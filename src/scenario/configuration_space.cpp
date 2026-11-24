#include "epycbench/scenario/configuration_space.hpp"
#include "scenario/package_scenario.hpp"

#include "epycbench/core/canonical_json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace epycbench::scenario {
namespace {
using Json = nlohmann::ordered_json;

auto error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.configuration_space.invalid"},
            .message = "Configuration space cannot be materialized",
            .operation = "scenario.configuration_space",
            .os_error = {},
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto known_keys(const Json& value, std::initializer_list<std::string_view> keys) -> bool {
    if (!value.is_object()) return false;
    for (auto field = value.begin(); field != value.end(); ++field)
        if (std::ranges::find(keys, field.key()) == keys.end()) return false;
    return true;
}

// Validate pointer structure before materialization, including when replaying a saved
// study without a catalog. Manifest admission checks the actual parameter names.
bool package_path(const Json& base, std::string_view path) {
    if (path == "/execution/pipeline_chunks") return has_stream_chunks(base);
    if (path == "/execution/repetitions" || path == "/execution/blocks" || path == "/execution/hardware_counters") return true;
    if (path.empty() || path.front() != '/') return false;
    path.remove_prefix(1);
    const auto first = path.find('/');
    if (first == path.npos) return false;
    const auto root = path.substr(0,first);
    path.remove_prefix(first+1);
    const auto boundary = path.find('/');
    if (boundary == path.npos || boundary == 0) return false;
    const auto id = path.substr(0,boundary);
    path.remove_prefix(boundary+1);
    if (root == "roles") {
        if (is_package_case_group(base)) return false;
        std::size_t index{};
        const auto parsed=std::from_chars(id.data(),id.data()+id.size(),index);
        if (parsed.ec != std::errc{} || parsed.ptr != id.data()+id.size() || std::to_string(index) != id ||
            index >= base.at("roles").size() || !base.at("roles").at(index).is_object()) return false;
        if (path == "cpus" || path == "numa_node" || path == "numa_nodes" || path == "implementation" ||
            path == "input_placement" || path == "service_cpu") return true;
        if (!path.starts_with("tuning/")) return false;
        path.remove_prefix(7);
    } else if (root == "workloads") {
        if (!base.at("workloads").contains(id) || !base.at("workloads").at(id).is_object() ||
            !path.starts_with("parameters/")) return false;
        path.remove_prefix(11);
        if (is_package_case_group(base) && std::ranges::any_of(base.at("cases"), [path](const auto& one) {
                return one.contains("parameters") && one.at("parameters").contains(path);
            })) return false;
    } else return false;
    return !path.empty() && path.find_first_of("/~") == path.npos;
}

} // namespace

auto objective_token(TaskObjective objective) noexcept -> std::string_view {
    return objective == TaskObjective::throughput ? "tasks_per_second" : "seconds_per_completed_task";
}

auto objective_orientation(TaskObjective objective) noexcept -> std::string_view {
    return objective == TaskObjective::throughput ? "maximize" : "minimize";
}

auto ConfigurationSpace::parse(const ScenarioSource& source, std::size_t maximum_candidates)
    -> std::expected<ConfigurationSpace, core::Error> {
    auto parsed = parse_authored_scenario(source);
    if (!parsed) return std::unexpected{std::move(parsed.error())};
    const auto& root = parsed->value();
    if (!root.is_object() || !root.contains("schema") || root.at("schema")!=package_space_schema)
        return std::unexpected{error("expected_configuration_space_v3_with_embedded_package_scenario")};
    if (!known_keys(root, {"schema", "objective", "scenario", "axes"}) || root.size() != 4U)
        return std::unexpected{error("document_shape")};
    if (maximum_candidates == 0U)
        return std::unexpected{error("candidate_limit")};

    const auto& objective_value = root.at("objective");
    TaskObjective objective;
    if (objective_value == objective_token(TaskObjective::throughput)) objective = TaskObjective::throughput;
    else if (objective_value == objective_token(TaskObjective::time_per_task)) objective = TaskObjective::time_per_task;
    else return std::unexpected{error("unknown_objective")};

    const auto& base = root.at("scenario");
    const auto& definitions = root.at("axes");
    if (!base.is_object() || !base.contains("schema") || base.at("schema") != package_scenario_schema ||
        (!is_package_case_group(base) && (!base.contains("roles") || !base.at("roles").is_array())) ||
        !base.contains("workloads") || !base.at("workloads").is_object() ||
        !base.contains("execution") || !base.at("execution").is_object() ||
        !definitions.is_array() || definitions.size() > 64U)
        return std::unexpected{error("package_recipe_shape")};
    std::set<std::string> paths;
    std::vector<Axis> axes;
    std::size_t count = 1;
    if (is_package_case_group(base)) {
        const auto cases = expand_package_cases(base);
        if (!cases) return std::unexpected{cases.error()};
        count = cases->size();
        if (count > maximum_candidates) return std::unexpected{error("candidate_limit")};
    }
    for (const auto& definition : definitions) {
        if (!known_keys(definition, {"path","values"}) || definition.size() != 2 ||
            !definition.at("path").is_string() || !definition.at("values").is_array())
            return std::unexpected{error("package_axis_shape")};
        const auto path = definition.at("path").get<std::string>();
        const auto& values = definition.at("values");
        if (!package_path(base,path)) {
            auto failure = error("unsupported_package_axis");
            failure.context["path"] = path;
            return std::unexpected{std::move(failure)};
        }
        if (!paths.insert(path).second || values.empty() || values.size() > maximum_candidates / count)
            return std::unexpected{error("invalid_or_oversized_axis")};
        std::set<std::string> unique;
        for (const auto& value : values)
            if (!unique.insert(value.dump()).second) return std::unexpected{error("duplicate_axis_choice")};
        try {
            axes.push_back({Json::json_pointer(path), values, is_package_input_axis(path)});
        } catch (const Json::exception&) { return std::unexpected{error("invalid_axis_pointer")}; }
        count *= values.size();
    }
    // CPU axes replace their base placements. Collect their alternatives once
    // without walking the Cartesian product of all the other axes.
    auto placements = base;
    std::set<core::LinuxCpuId> occupied;
    for (const auto& axis : axes) {
        const auto& field = axis.path.back();
        if (field != "cpus" && field != "service_cpu")
            continue;
        const auto role = axis.path.parent_pointer();
        if (role.parent_pointer() != Json::json_pointer{"/roles"})
            continue;
        placements.at(role).erase(field);
        for (const auto& choice : axis.choices) {
            if (field == "service_cpu") {
                collect_package_occupied_cpu(choice, occupied);
            } else if (choice.is_array()) {
                for (const auto& cpu : choice)
                    collect_package_occupied_cpu(cpu, occupied);
            }
        }
    }
    collect_package_occupied_cpus(placements, occupied);
    return ConfigurationSpace{objective, base, std::move(axes), std::move(occupied), count};
}

auto ConfigurationSpace::axis_cardinalities() const -> std::vector<std::size_t> {
    std::vector<std::size_t> cardinalities;
    cardinalities.reserve(axes_.size());
    if (is_package_case_group(scenario_)) cardinalities.push_back(scenario_.at("cases").size());
    for (const auto& axis : axes_) cardinalities.push_back(axis.choices.size());
    return cardinalities;
}

auto ConfigurationSpace::describe_axes() const -> Json {
    auto result = Json::array();
    if (is_package_case_group(scenario_)) {
        auto choices = Json::array();
        for (const auto& one : scenario_.at("cases")) choices.push_back(one.at("id").dump());
        result.push_back({{"scenario_paths", Json::array({"/case_id"})}, {"choices_json", std::move(choices)}});
    }
    for (const auto& axis : axes_) {
        auto choices = Json::array();
        for (const auto& choice : axis.choices) choices.push_back(choice.dump());
        auto paths = Json::array({axis.path.to_string()});
        result.push_back({{"scenario_paths", std::move(paths)}, {"choices_json", std::move(choices)}});
    }
    return result;
}

auto ConfigurationSpace::varies_task_parameters() const noexcept -> bool {
    return (is_package_case_group(scenario_) && scenario_.at("cases").size() > 1U) ||
           std::ranges::any_of(axes_, [](const Axis& axis) {
        return axis.task_parameter && axis.choices.size() > 1U;
    });
}

auto ConfigurationSpace::package_selections() const -> std::set<std::pair<std::string, std::string>> {
    std::set<std::pair<std::string, std::string>> selections;
    const auto& workloads = scenario_.at("workloads");
    const auto collect = [&](const Json& role, const Json& implementation) {
        const auto* id = implementation.get_ptr<const std::string*>();
        const auto workload = role.find("workload");
        if (!id || workload == role.end() || !workload->is_string()) return;
        const auto source = workloads.find(workload->get_ref<const std::string&>());
        if (source == workloads.end()) return;
        const auto task = source->find("task");
        if (task != source->end() && task->is_string())
            selections.emplace(task->get_ref<const std::string&>(), *id);
    };
    const auto collect_roles = [&](const Json& roles) {
        for (const auto& role : roles) {
            const auto implementation = role.find("implementation");
            if (implementation != role.end()) collect(role, *implementation);
        }
    };
    if (is_package_case_group(scenario_)) {
        for (const auto& one : scenario_.at("cases")) collect_roles(one.at("roles"));
    } else {
        // Resolution also checks the embedded base when an axis replaces its selection.
        collect_roles(scenario_.at("roles"));
        for (const auto& axis : axes_) {
            const auto role = axis.path.parent_pointer();
            if (axis.path.back() != "implementation" || role.parent_pointer() != Json::json_pointer{"/roles"})
                continue;
            for (const auto& choice : axis.choices) collect(scenario_.at(role), choice);
        }
    }
    return selections;
}

auto ConfigurationSpace::materialize(std::size_t ordinal) const
    -> std::expected<Json, core::Error> {
    if (ordinal >= size_) return std::unexpected{error("candidate_ordinal")};
    try {
        auto result = is_package_case_group(scenario_)
            ? Json(materialize_package_case(scenario_, ordinal / (size_ / scenario_.at("cases").size())))
            : scenario_;
        for (auto axis = axes_.rbegin(); axis != axes_.rend(); ++axis) {
            const auto& choice = axis->choices.at(ordinal % axis->choices.size());
            result[axis->path] = choice;
            ordinal /= axis->choices.size();
        }
        return result;
    } catch (const Json::exception&) { return std::unexpected{error("invalid_axis_target")}; }
}

auto ConfigurationSpace::resolve_controller(const ResourceResolutionContext& topology) const
    -> std::expected<PackageControllerPlacement, core::Error> {
    return choose_package_controller(scenario_.at("execution"), occupied_cpus_, topology);
}

auto ConfigurationSpace::resolve(std::size_t ordinal, const ResourceResolutionContext& topology,
                                 std::vector<std::string>* related_settings) const
    -> std::expected<ConfigurationCandidate, core::Error> {
    if (related_settings) related_settings->clear();
    if (ordinal >= size_) return std::unexpected{error("candidate_ordinal")};
    if (!topology.packages) return std::unexpected{error("installed_package_catalog_required")};
    // A sweep embeds a usable Scenario, even when an axis replaces an invalid
    // base value in every materialized candidate.
    const nlohmann::json base_source = is_package_case_group(scenario_)
        ? materialize_package_case(scenario_, ordinal / (size_ / scenario_.at("cases").size()))
        : nlohmann::json(scenario_);
    auto authored = materialize(ordinal);
    if (!authored) return std::unexpected{authored.error()};
    const bool matches_base = nlohmann::json(*authored) == base_source;
    if (!matches_base) {
        auto base = validate_package_sweep_base(base_source, *topology.packages, topology);
        if (!base) return std::unexpected{base.error()};
    }
    auto settings = package_axes(base_source, *topology.packages);
    if (!settings) return std::unexpected{settings.error()};
    std::vector<std::string> sweep_axes;
    for (const auto& axis : axes_) {
        auto path = axis.path.to_string();
        if (!std::ranges::contains(*settings, path)) {
            auto failure = error("unsupported_package_axis");
            failure.context["path"] = path;
            if (related_settings) related_settings->push_back(std::move(path));
            return std::unexpected{std::move(failure)};
        }
        if (related_settings && !matches_base) sweep_axes.push_back(std::move(path));
    }
    auto controller = resolve_controller(topology);
    if (!controller) {
        if (related_settings) {
            const auto conflicts = controller.error().context.find("conflicting_cpus");
            const auto& execution = scenario_.at("execution");
            const auto free_cores = !execution.contains("controller_core")
                ? package_controller_choices(nlohmann::json::object(), topology)
                : std::vector<PackageControllerPlacement>{};
            const auto contributes = [&](const Json& cpu) {
                if (!cpu.is_number_unsigned()) return false;
                if (conflicts != controller.error().context.end())
                    return std::ranges::contains(*conflicts, nlohmann::json(cpu));
                if (free_cores.empty()) return false;
                const auto units = topology.topology.processing_units();
                const auto unit = std::ranges::find_if(units, [&](const auto& item) {
                    return item.linux_cpu_id.value() == cpu.get<std::uint64_t>();
                });
                return unit != units.end() && std::ranges::any_of(free_cores, [&](const auto& candidate) {
                    const auto core = topology.stable_ids.core(candidate.core);
                    return core && *core == unit->core_id;
                });
            };
            for (const auto& axis : axes_) {
                const auto& field = axis.path.back();
                if (field == "cpus" && std::ranges::any_of(axis.choices, [&](const Json& option) {
                        return option.is_array() && std::ranges::any_of(option, contributes);
                    }))
                    related_settings->push_back(axis.path.to_string());
                if (field == "service_cpu" && std::ranges::any_of(axis.choices, contributes))
                    related_settings->push_back(axis.path.to_string());
            }
        }
        return std::unexpected{controller.error()};
    }
    (*authored)["execution"]["controller_core"] = controller->core.value();
    ConfigurationCandidate candidate{ordinal,std::move(*authored),{},{},{},{},{}};
    auto plan = resolve_package(candidate.authored_scenario, *topology.packages, topology,
        matches_base ? nullptr : related_settings, sweep_axes);
    if (!plan) {
        if (matches_base) return std::unexpected{std::move(plan.error())};
        const auto& failure = plan.error();
        candidate.diagnostics.push_back({failure.code,failure.context.value("path",""),{},{{"reason",failure.message}}});
    } else {
        candidate.input_identity = plan->input_identity;
        candidate.problem_identity = plan->problem_identity;
        candidate.candidate_identity = plan->candidate_identity;
        candidate.package.emplace(std::move(*plan));
    }
    return candidate;
}
} // namespace epycbench::scenario
