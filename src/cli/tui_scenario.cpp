#include "cli/tui.hpp"

#include "cli/run_output_directory.hpp"
#include "cli/scenario_guides.hpp"
#include "cli/tui_placement.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <string_view>
#include <utility>

namespace epycbench::cli::tui {
namespace {

using kernel::packages::ImplementationManifest;
using kernel::packages::MappingResource;
using kernel::packages::ParameterDefinition;
using kernel::packages::ParameterDefinitions;
using kernel::packages::ParameterType;
using kernel::packages::TaskManifest;

constexpr std::array<std::string_view, 4> modes{"standalone", "comparison", "mixed", "pipeline"};
constexpr std::array<std::string_view, 4> mode_labels{
    "Standalone", "Comparison", "Concurrent Workloads", "Pipeline"};
constexpr std::string_view task_payload = "scenario-task:";
constexpr std::string_view implementation_payload = "scenario-implementation:";
constexpr std::string_view output_payload = "pipeline-output:";

auto role_path(std::size_t index) -> std::string { return "/roles/" + std::to_string(index); }

auto task_for(const Session& s, std::string_view id) -> const TaskManifest* {
    const auto packages = s.catalog->implementations();
    const auto found = std::ranges::find_if(packages, [id](const auto& package) {
        return package.task->id == id;
    });
    return found == packages.end() ? nullptr : found->task.get();
}

auto workload_for(const Session& s, const Json& role) -> const Json& {
    return s.draft.at("workloads").at(role.at("workload").get_ref<const std::string&>());
}

auto task_for_role(const Session& s, const Json& role) -> const TaskManifest* {
    return task_for(s, workload_for(s, role).at("task").get_ref<const std::string&>());
}

auto implementation_for(const Session& s, const Json& role) -> const ImplementationManifest* {
    return s.catalog->find(workload_for(s, role).at("task").get_ref<const std::string&>(),
                           role.at("implementation").get_ref<const std::string&>());
}

auto role_label(const Json& source, std::size_t index) -> std::string {
    const auto& mode = source.at("mode");
    if (mode == "comparison") return index == 0U ? "Baseline" : "Candidate";
    return (mode == "pipeline" ? "Stage " : "Role ") + std::to_string(index + 1U);
}

auto role_placeholder(const Json& source, std::size_t index) -> Json {
    const auto id = source.at("mode") == "comparison"
        ? (index == 0U ? std::string{"baseline"} : std::string{"candidate"})
        : "role-" + std::to_string(index + 1U);
    return {{"id", id}, {"workload", source.at("mode") == "comparison" ? "shared" : id},
            {"implementation", ""}, {"tuning", Json::object()}, {"cpus", Json::array()}};
}

void add_role(Json& source) {
    auto& roles = source.at("roles");
    auto& workloads = source.at("workloads");
    auto ordinal = roles.size();
    auto role = role_placeholder(source, ordinal);
    if (source.at("mode") != "comparison") {
        while (workloads.contains(role.at("workload").get_ref<const std::string&>()) ||
               std::ranges::any_of(roles, [&role](const auto& used) {
                   return used.at("id") == role.at("id");
               })) role = role_placeholder(source, ++ordinal);
    }
    const auto& name = role.at("workload").get_ref<const std::string&>();
    if (!workloads.contains(name)) workloads[name] = {{"task", ""}, {"parameters", Json::object()}};
    roles.push_back(std::move(role));
}

void show_editor(Session& s, std::string message = {}) {
    edit_scenario(s);
    if (!message.empty()) s.message = std::move(message);
}

void changed(Session& s) {
    s.message.clear();
    s.persist();
    show_editor(s, std::move(s.message));
}

auto type_label(ParameterType type) -> std::string_view {
    switch (type) {
        case ParameterType::uint64: return "uint64";
        case ParameterType::int64: return "int64";
        case ParameterType::float64: return "float64";
        case ParameterType::boolean: return "boolean";
        case ParameterType::enumeration: return "enum";
    }
    return {};
}

auto setting_detail(const ParameterDefinition& spec, const Json* value) -> std::string {
    std::string detail;
    if (value) detail = value->dump();
    else if (spec.default_value) detail = spec.default_value->dump() + " (default)";
    else detail = "Required";
    detail += "  ";
    detail += type_label(spec.type);
    if (spec.minimum) detail += "  min " + spec.minimum->dump();
    if (spec.maximum) detail += "  max " + spec.maximum->dump();
    if (!spec.values.empty()) detail += "  " + Json(spec.values).dump();
    return detail;
}

void parameter(Session& s, const std::string& panel, const Json::json_pointer& object,
               const std::string& name, const ParameterDefinition& spec) {
    const Json* current = nullptr;
    if (s.draft.contains(object)) {
        const auto& values = s.draft.at(object);
        if (const auto found = values.find(name); found != values.end()) current = &*found;
    }
    const auto detail = setting_detail(spec, current);
    s.field_action(name, [&s, object, name, spec, initial = current ? current->dump() : std::string{}] {
        const auto cancel = [&s] { show_editor(s); };
        const auto set = [&s, object, name, spec](const std::optional<Json>& value) {
            if (!value) {
                if (s.draft.contains(object)) s.draft.at(object).erase(name);
                changed(s);
                return;
            }
            const ParameterDefinitions definitions{{name, spec}};
            auto checked = kernel::packages::resolve_parameters(definitions, {{name, *value}}, name);
            if (!checked) {
                show_editor(s, checked.error().message);
                return;
            }
            s.draft[object][name] = checked->at(name);
            changed(s);
        };
        if (spec.type == ParameterType::enumeration || spec.type == ParameterType::boolean) {
            auto choices = spec.type == ParameterType::boolean
                ? std::vector<std::string>{"false", "true"} : spec.values;
            if (spec.default_value) choices.push_back("Use default (" + spec.default_value->dump() + ")");
            s.choose(name, std::move(choices), [set, spec](std::size_t selected) {
                const auto count = spec.type == ParameterType::boolean ? 2U : spec.values.size();
                if (selected == count) set(std::nullopt);
                else if (spec.type == ParameterType::boolean) set(Json(selected == 1U));
                else set(Json(spec.values.at(selected)));
            }, cancel);
            return;
        }
        s.input(name + " (" + std::string{type_label(spec.type)} + ")", initial,
                [&s, set, spec](std::string text) {
            if (text.empty() && spec.default_value) {
                set(std::nullopt);
                return;
            }
            auto value = Json::parse(text, nullptr, false);
            if (value.is_discarded()) {
                show_editor(s, "Enter a value of the declared type.");
                return;
            }
            set(std::move(value));
        }, cancel);
    }, detail, panel, (object / name).to_string());
}

void task_parameters(Session& s, const std::string& name, const std::string& panel) {
    const auto& workload = s.draft.at("workloads").at(name);
    const auto& id = workload.at("task").get_ref<const std::string&>();
    const auto* task = task_for(s, id);
    if (!task) return;
    const auto parameters = Json::json_pointer("/workloads") / name / "parameters";
    if (task->parameters.empty()) s.note("Task inputs", "None declared", panel);
    for (const auto& [key, spec] : task->parameters) parameter(s, panel, parameters, key, spec);
}

auto compatible_ports(const TaskManifest& previous, const TaskManifest& task)
    -> std::map<std::string, std::vector<std::string>> {
    std::map<std::string, std::vector<std::string>> ports;
    for (const auto& [input, type] : task.ports.inputs)
        for (const auto& [output, format] : previous.ports.outputs)
            if (type == format) ports[input].push_back(output);
    return ports;
}

void refresh_links(Session& s, std::size_t index) {
    auto& roles = s.draft.at("roles");
    if (index == 0U || index == roles.size()) return;
    auto& role = roles.at(index);
    const auto* previous = task_for_role(s, roles.at(index - 1U));
    const auto* task = task_for_role(s, role);
    if (!previous || !task) {
        role.erase("inputs");
        return;
    }
    const auto ports = compatible_ports(*previous, *task);
    auto links = Json::object();
    std::size_t pairs{};
    for (const auto& [input, outputs] : ports) {
        pairs += outputs.size();
        if (!role.contains("inputs") || !role.at("inputs").contains(input)) continue;
        const auto& current = role.at("inputs").at(input);
        if (current.is_string() && std::ranges::contains(outputs, current.get_ref<const std::string&>()))
            links[input] = current;
    }
    if (pairs == 1U) links[ports.begin()->first] = ports.begin()->second.front();
    if (links.empty()) role.erase("inputs");
    else role["inputs"] = std::move(links);
}

auto compatible_task(const Session& s, const TaskManifest& task, std::size_t index) -> bool {
    const auto& mode = s.draft.at("mode");
    if (mode == "standalone") return true;
    if (task.mapping != MappingResource::none) return false;
    if (mode != "pipeline") return true;
    const auto& roles = s.draft.at("roles");
    if ((index != 0U && task.ports.inputs.empty()) ||
        (index + 1U != roles.size() && task.ports.outputs.empty()))
        return false;
    if (index != 0U) {
        const auto* previous = task_for_role(s, roles.at(index - 1U));
        if (previous && compatible_ports(*previous, task).empty()) return false;
    }
    if (index + 1U != roles.size()) {
        const auto* next = task_for_role(s, roles.at(index + 1U));
        if (next && compatible_ports(task, *next).empty()) return false;
    }
    return true;
}

void assign_task(Session& s, std::size_t index, const std::string& id) {
    const auto name = s.draft.at("roles").at(index).at("workload").get<std::string>();
    auto& workload = s.draft.at("workloads").at(name);
    if (workload.at("task") == id) {
        show_editor(s);
        return;
    }
    workload = {{"task", id}, {"parameters", Json::object()}};
    for (auto& role : s.draft.at("roles")) {
        if (role.at("workload") != name) continue;
        role["implementation"] = "";
        role["tuning"] = Json::object();
        role.erase("service_cpu");
    }
    if (s.draft.at("mode") == "pipeline") {
        refresh_links(s, index);
        refresh_links(s, index + 1U);
    }
    changed(s);
}

auto task_ids(const Session& s) -> std::vector<std::string> {
    std::vector<std::string> tasks;
    for (const auto& package : s.catalog->implementations())
        if (!std::ranges::contains(tasks, package.task->id)) tasks.push_back(package.task->id);
    return tasks;
}

void task_slot(Session& s, std::size_t index, const std::string& panel, const std::string& label) {
    const auto& role = s.draft.at("roles").at(index);
    const auto& id = workload_for(s, role).at("task").get_ref<const std::string&>();
    auto& slot = s.action(label, [&s, index] {
        auto choices = task_ids(s);
        std::erase_if(choices, [&s, index](const auto& choice) {
            return !compatible_task(s, *task_for(s, choice), index);
        });
        if (choices.empty()) {
            show_editor(s, "No installed task has compatible ports and execution mode.");
            return;
        }
        s.choose("Choose task", choices, [&s, index, choices](std::size_t pick) {
            assign_task(s, index, choices.at(pick));
        }, [&s] { show_editor(s); });
    }, id.empty() ? "Choose from the installed tasks" : id, panel);
    slot.accepts = [&s, index](const std::string& payload) {
        if (!payload.starts_with(task_payload)) return false;
        const auto* task = task_for(s, std::string_view{payload}.substr(task_payload.size()));
        return task && compatible_task(s, *task, index);
    };
    slot.drop = [&s, index](std::string payload) {
        assign_task(s, index, payload.substr(task_payload.size()));
    };
    slot.rejection = [&s](const std::string& payload) -> std::string {
        if (!payload.starts_with(task_payload)) return "Pick up an installed task for this slot.";
        if (s.draft.at("mode") == "pipeline")
            return "Choose a task with ports compatible with the neighboring stages.";
        return "This task uses a fresh mapping and requires Standalone mode.";
    };
}

auto supports_inputs(const ImplementationManifest& package, const Json& workload) -> bool {
    const auto settings = kernel::packages::resolve_parameters(
        package.task->parameters, workload.value("parameters", Json::object()), "parameters");
    return !settings || kernel::packages::validate_implementation_inputs(
        package.accepts, *settings, "parameters").has_value();
}

auto implementation_from_payload(const Session& s, const std::string& payload)
    -> const ImplementationManifest* {
    if (!payload.starts_with(implementation_payload)) return nullptr;
    const auto words = std::string_view{payload}.substr(implementation_payload.size());
    const auto split = words.find('\n');
    if (split == std::string_view::npos) return nullptr;
    return s.catalog->find(words.substr(0, split), words.substr(split + 1U));
}

void assign_implementation(Session& s, std::size_t index, const ImplementationManifest& package) {
    auto& role = s.draft.at("roles").at(index);
    if (role.at("implementation") == package.id) {
        show_editor(s);
        return;
    }
    role["implementation"] = package.id;
    role["tuning"] = Json::object();
    if (package.task->mapping == MappingResource::allocation) {
        role.erase("numa_node");
        role.erase("numa_nodes");
    }
    if (package.task->mapping != MappingResource::none) {
        role["service_cpu"] = nullptr;
        role.erase("input_placement");
        auto& execution = s.draft.at("execution");
        execution["repetitions"] = 1U;
        execution["hardware_counters"] = false;
        execution.erase("calibration");
        execution.erase("warmup");
    }
    changed(s);
}

void implementation_slot(Session& s, std::size_t index, const std::string& panel) {
    const auto& role = s.draft.at("roles").at(index);
    const auto& workload = workload_for(s, role);
    const auto& id = role.at("implementation").get_ref<const std::string&>();
    const auto* implementation = implementation_for(s, role);
    auto detail = id.empty() ? std::string{"Choose a compatible implementation"} : id;
    if (implementation && !supports_inputs(*implementation, workload))
        detail += "  Does not accept the current task inputs";
    auto& slot = s.action("Implementation", [&s, index] {
        const auto& current = workload_for(s, s.draft.at("roles").at(index));
        std::vector<std::string> choices;
        for (const auto& package : s.catalog->implementations())
            if (current.at("task") == package.task->id && supports_inputs(package, current))
                choices.push_back(package.id);
        if (choices.empty()) {
            show_editor(s, "No installed implementation accepts the selected task and inputs.");
            return;
        }
        s.choose("Choose implementation", choices, [&s, index, choices](std::size_t pick) {
            const auto& selected = workload_for(s, s.draft.at("roles").at(index));
            const auto* package = s.catalog->find(selected.at("task").get_ref<const std::string&>(),
                                                  choices.at(pick));
            assign_implementation(s, index, *package);
        }, [&s] { show_editor(s); });
    }, detail, panel);
    slot.accepts = [&s, index](const std::string& payload) {
        const auto* package = implementation_from_payload(s, payload);
        const auto& current = workload_for(s, s.draft.at("roles").at(index));
        return package && current.at("task") == package->task->id && supports_inputs(*package, current);
    };
    slot.drop = [&s, index](std::string payload) {
        if (const auto* package = implementation_from_payload(s, payload))
            assign_implementation(s, index, *package);
    };
    slot.rejection = [&s, index](const std::string& payload) -> std::string {
        const auto* package = implementation_from_payload(s, payload);
        if (!package) return "Pick up an installed implementation for this role.";
        const auto& current = workload_for(s, s.draft.at("roles").at(index));
        if (current.at("task") != package->task->id)
            return "Choose an implementation of " + current.at("task").get<std::string>() + ".";
        return "This implementation does not accept the current task inputs.";
    };
    if (!implementation) return;
    for (const auto& [name, spec] : implementation->tuning)
        parameter(s, panel, Json::json_pointer(role_path(index) + "/tuning"), name, spec);
}

void assign_link(Session& s, std::size_t index, const std::string& input, const std::string& output) {
    auto& role = s.draft.at("roles").at(index);
    if (output.empty()) {
        if (role.contains("inputs")) {
            auto& links = role.at("inputs");
            links.erase(input);
            if (links.empty()) role.erase("inputs");
        }
    } else {
        role["inputs"][input] = output;
    }
    changed(s);
}

void pipeline_ports(Session& s, std::size_t index, const std::string& panel) {
    const auto& roles = s.draft.at("roles");
    const auto& role = roles.at(index);
    const auto* task = task_for_role(s, role);
    if (!task) return;
    const auto* previous = index == 0U ? nullptr : task_for_role(s, roles.at(index - 1U));
    const auto ports = previous ? compatible_ports(*previous, *task)
                                : std::map<std::string, std::vector<std::string>>{};
    for (const auto& [input, format] : task->ports.inputs) {
        const auto choices = ports.find(input);
        if (choices == ports.end()) {
            s.note("Input " + input, format + "  Generated", panel);
            continue;
        }
        const auto linked = role.value("inputs", Json::object()).value(input, "");
        const auto detail = format + (linked.empty() ? "  Generated"
            : "  From stage " + std::to_string(index) + " output " + linked);
        auto& target = s.action("Input " + input, [&s, index, input, outputs = choices->second] {
            auto labels = outputs;
            labels.emplace_back("Generate this input");
            s.choose("Connect input " + input, std::move(labels),
                     [&s, index, input, outputs](std::size_t pick) {
                assign_link(s, index, input, pick == outputs.size() ? std::string{} : outputs.at(pick));
            }, [&s] { show_editor(s); });
        }, detail, panel);
        const auto prefix = std::string{output_payload} + std::to_string(index - 1U) + ":";
        target.accepts = [prefix, outputs = choices->second](const std::string& payload) {
            return payload.starts_with(prefix) && std::ranges::contains(outputs, payload.substr(prefix.size()));
        };
        target.drop = [&s, index, input, prefix](std::string payload) {
            assign_link(s, index, input, payload.substr(prefix.size()));
        };
        target.rejection = [format, index](const std::string&) {
            return "Choose a " + format + " output from stage " + std::to_string(index) + ".";
        };
    }
    for (const auto& [output, format] : task->ports.outputs) {
        s.note("Output " + output, format, panel);
        if (index + 1U != roles.size())
            s.rows.back().payload = std::string{output_payload} + std::to_string(index) + ":" + output;
    }
}

auto placement_summary(const Json& role) -> std::string {
    const auto cpus = role.value("cpus", Json::array());
    if (cpus.empty()) return "Choose ordered worker CPUs and memory";
    std::string detail = "CPUs " + cpus.dump();
    if (role.contains("numa_node")) detail += "  NUMA " + role.at("numa_node").dump();
    if (role.contains("numa_nodes")) detail += "  NUMA " + role.at("numa_nodes").dump();
    if (role.contains("service_cpu") && !role.at("service_cpu").is_null())
        detail += "  Service CPU " + role.at("service_cpu").dump();
    return detail;
}

void package_catalog(Session& s) {
    const auto tasks = task_ids(s);
    for (const auto& task : tasks) {
        s.note(task, {}, "Installed tasks");
        s.rows.back().payload = std::string{task_payload} + task;
    }
    for (const auto& package : s.catalog->implementations()) {
        const auto& workloads = s.draft.at("workloads");
        if (std::ranges::none_of(workloads, [&](const auto& work) {
            return work.at("task") == package.task->id;
        })) continue;
        s.note(package.id, package.task->id, "Installed implementations");
        s.rows.back().payload = std::string{implementation_payload} + package.task->id + "\n" + package.id;
    }
}

auto unsigned_number(std::string_view text, std::uint64_t low, std::uint64_t high)
    -> std::optional<std::uint64_t> {
    if (text.empty()) return {};
    std::uint64_t value{};
    const auto end = text.data() + text.size();
    const auto decoded = std::from_chars(text.data(), end, value);
    if (decoded.ec != std::errc{} || decoded.ptr != end || value < low || value > high) return {};
    return value;
}

void execution_number(Session& s, std::string field, std::string label,
                      std::uint64_t low, std::uint64_t high, bool optional = false) {
    const auto& execution = s.draft.at("execution");
    const auto current = execution.contains(field) ? execution.at(field).dump() : std::string{};
    s.field_action(label, [&s, field, label, current, low, high, optional] {
        s.input(label, current, [&s, field, low, high, optional](std::string text) {
            if (optional && text.empty()) {
                s.draft.at("execution").erase(field);
                changed(s);
                return;
            }
            if (field == "pipeline_chunks" && s.draft.at("execution").value("hardware_counters", false)) {
                show_editor(s, "Disable worker hardware counters before choosing pipeline chunks.");
                return;
            }
            const auto value = unsigned_number(text, low, high);
            if (!value) {
                show_editor(s, "Enter an integer from " + std::to_string(low) + " to " +
                               std::to_string(high) + ".");
                return;
            }
            s.draft.at("execution")[field] = *value;
            changed(s);
        }, [&s] { show_editor(s); });
    }, (current.empty() ? std::string{"Unset"} : current) + "  " +
       std::to_string(low) + " to " + std::to_string(high), "Execution");
}

void execution_options(Session& s) {
    s.page("Observation controls", [&s] { show_editor(s); });
    const auto& mode = s.draft.at("mode");
    const auto* task = mode == "standalone" ? task_for_role(s, s.draft.at("roles").at(0)) : nullptr;
    const bool fresh = task && task->mapping != MappingResource::none;
    std::vector<std::pair<std::string, std::string>> options;
    if (!fresh && (mode == "standalone" || mode == "comparison" || mode == "cases")) {
        options.emplace_back("calibration", "Calibration");
        options.emplace_back("warmup", "Warmup");
    }
    if (mode == "comparison" || mode == "cases") options.emplace_back("precision", "Ratio precision");
    options.emplace_back("monitoring", "Monitoring");
    for (const auto& [field, label] : options) {
        const auto& execution = s.draft.at("execution");
        const auto current = execution.value(field, Json());
        auto input = current;
        if (field == "monitoring" && input.is_object()) input.erase("service_cpu");
        s.field_action(label, [&s, field, label, initial = input.is_null() ? "" : input.dump()] {
            s.input(label + " JSON object (empty to disable)", initial, [&s, field](std::string text) {
                auto& settings = s.draft.at("execution");
                if (text.empty()) {
                    settings.erase(field);
                } else {
                    auto value = Json::parse(text, nullptr, false);
                    if (!value.is_object() || (field == "monitoring" && value.contains("service_cpu"))) {
                        execution_options(s);
                        s.message = field == "monitoring"
                            ? "Enter a JSON object. Choose the monitoring service CPU in Placement."
                            : "Enter a JSON object or leave it empty.";
                        return;
                    }
                    if (field == "monitoring" && settings.contains(field) &&
                        settings.at(field).contains("service_cpu"))
                        value["service_cpu"] = settings.at(field).at("service_cpu");
                    settings[field] = std::move(value);
                }
                s.message.clear();
                s.persist();
                auto message = std::move(s.message);
                execution_options(s);
                s.message = std::move(message);
            }, [&s] { execution_options(s); });
        }, current.is_null() ? "Disabled" : current.dump(), "Execution");
    }
}

void execution_panel(Session& s) {
    const auto& mode = s.draft.at("mode");
    const auto& execution = s.draft.at("execution");
    const auto* task = mode == "standalone" ? task_for_role(s, s.draft.at("roles").at(0)) : nullptr;
    const bool fresh = task && task->mapping != MappingResource::none;
    const bool streaming = execution.contains("pipeline_chunks");
    if (fresh)
        s.note("Repetitions per callback", "1 for fresh mappings", "Execution");
    else
        execution_number(s, "repetitions", "Repetitions per callback", 1U,
            streaming && execution.at("pipeline_chunks").is_number_unsigned() &&
            execution.at("pipeline_chunks") >= 2U && execution.at("pipeline_chunks") <= 64U
                ? 64U / execution.at("pipeline_chunks").get<std::uint64_t>() : 1000000000U);
    execution_number(s, "blocks", "Measured blocks", 1U, 4096U);
    execution_number(s, "order_seed", "Order seed", 0U, UINT64_MAX, true);
    if (!fresh && !streaming) {
        const auto counters = execution.value("hardware_counters", false);
        s.action("Worker hardware counters", [&s, counters] {
            s.draft.at("execution")["hardware_counters"] = !counters;
            changed(s);
        }, counters ? "Enabled" : "Disabled", "Execution");
    }
    if (mode == "pipeline" && s.draft.at("roles").size() == 2U)
        execution_number(s, "pipeline_chunks", "Pipeline chunks (empty for staged execution)", 2U, 64U, true);
    s.action("Observation controls", [&s] { execution_options(s); }, {}, "Execution");
    const auto controller = execution.value("controller_core", std::string{"Automatic"});
    s.action("Controller and monitoring placement", [&s] {
        placement(s, {}, [&s] { show_editor(s); });
    }, controller, "Execution");
    const auto directory = s.draft.contains("output")
        ? s.draft.at("output").value("directory", "") : std::string{};
    s.field_action("Result directory", [&s, directory] {
        s.input("Absolute private Result directory (empty to omit)", directory, [&s](std::string path) {
            if (path.empty()) {
                s.draft.erase("output");
            } else if (run_output_directory::valid(path)) {
                s.draft["output"] = {{"directory", std::move(path)}, {"format", "json"}};
            } else {
                show_editor(s, "Enter an absolute Result directory.");
                return;
            }
            changed(s);
        }, [&s] { show_editor(s); });
    }, directory.empty() ? "Omitted" : directory, "Execution");
}

auto diagnostic(const core::Error& error) -> std::string {
    const auto path = error.context.value("path", std::string{});
    return (path.empty() ? std::string{} : path + "  ") + error.message;
}

auto validate(const Session& s) -> std::expected<std::string, std::string> {
    if (s.draft.contains("output")) {
        const auto directory = s.draft.at("output").value("directory", "");
        if (!run_output_directory::valid(directory))
            return std::unexpected{"Choose an absolute Result directory."};
    }
    if (scenario::is_package_case_group(s.draft)) {
        auto cases = scenario::resolve_package_cases(s.draft, *s.catalog, s.context());
        if (!cases) return std::unexpected{diagnostic(cases.error())};
        return std::to_string(cases->size()) + " independent comparison cases resolved.";
    }
    auto plan = scenario::resolve_package(s.draft, *s.catalog, s.context());
    if (!plan) return std::unexpected{diagnostic(plan.error())};
    if (!plan->controller) return "Scenario resolved.";
    return "Scenario resolved. Controller Linux CPU " +
        std::to_string(plan->controller->cpu.value()) + " on core " +
        std::string{plan->controller->core.value()} + ".";
}

void save_scenario(Session& s) {
    const auto status = validate(s);
    if (!status) {
        show_editor(s, status.error());
        return;
    }
    const auto filename = s.artifact.empty()
        ? std::string{"scenario.json"} : s.artifact.stem().string() + "-edited.json";
    s.input("Save new Scenario", (s.scenario_dir / filename).string(), [&s](std::string name) {
        if (name.empty() || name.contains('\0')) {
            show_editor(s, "Enter a filename for the new Scenario.");
            return;
        }
        Path path{std::move(name)};
        if (!path.has_filename() || path.filename() == "." || path.filename() == "..") {
            show_editor(s, "Enter a filename for the new Scenario.");
            return;
        }
        if (!path.has_extension()) path += ".json";
        const auto extension = path.extension();
        if (extension != ".json" && extension != ".yaml" && extension != ".yml") {
            show_editor(s, "Use a .json, .yaml or .yml Scenario filename.");
            return;
        }
        if (!path.has_parent_path()) {
            if (s.scenario_dir.empty()) {
                show_editor(s, "The default save directory is unavailable. Enter a file path.");
                return;
            }
            path = s.scenario_dir / path;
        }
        if (!s.save(path, s.draft)) {
            show_editor(s, std::move(s.message));
            return;
        }
        s.message.clear();
        s.clear_draft();
        auto failure = std::move(s.message);
        s.page("Saved Scenario", [&s] { s.home(); });
        s.message = std::move(failure);
        s.note(path.string(), {}, "Scenario");
        s.action("Create a sweep from this Scenario", [&s, path] { open_sweep(s, path); });
        s.action("Home", [&s] { s.home(); });
    }, [&s] { show_editor(s); });
    s.message = *status;
}

void case_editor(Session& s, std::size_t ordinal) {
    const auto& item = s.draft.at("cases").at(ordinal);
    s.page("Case " + item.at("id").get<std::string>(), [&s] { show_editor(s); });
    if (item.contains("parameters"))
        s.note("Case input overrides", item.at("parameters").dump(), "Workload");
    for (std::size_t index = 0; index < item.at("roles").size(); ++index) {
        const auto& role = item.at("roles").at(index);
        const auto panel = index == 0U ? "Baseline" : "Candidate";
        s.note("Implementation", role.at("implementation").get<std::string>(), panel);
        s.action("Placement", [&s, index, ordinal] {
            placement(s, "/cases/" + std::to_string(ordinal) + role_path(index),
                      [&s, ordinal] { case_editor(s, ordinal); });
        }, placement_summary(role), panel);
    }
}

void case_panel(Session& s) {
    const auto& workloads = s.draft.at("workloads");
    task_parameters(s, workloads.begin().key(), "Shared task inputs");
    const auto count = s.draft.at("cases").size();
    s.action("Comparison cases", [&s] {
        std::vector<std::string> labels;
        for (const auto& item : s.draft.at("cases")) labels.push_back(item.at("id").get<std::string>());
        s.choose("Comparison cases", std::move(labels), [&s](std::size_t ordinal) {
            case_editor(s, ordinal);
        }, [&s] { show_editor(s); });
    }, std::to_string(count) + " independent comparisons", "Cases");
}

bool editable_role(const Json& role, const Json& workloads) {
    return role.is_object() && role.contains("id") && role.at("id").is_string() &&
           role.contains("workload") && role.at("workload").is_string() &&
           workloads.contains(role.at("workload").get_ref<const std::string&>()) &&
           role.contains("implementation") && role.at("implementation").is_string() &&
           (!role.contains("cpus") || role.at("cpus").is_array()) &&
           (!role.contains("tuning") || role.at("tuning").is_object()) &&
           (!role.contains("inputs") || (role.at("inputs").is_object() &&
             std::ranges::all_of(role.at("inputs"), [](const auto& link) { return link.is_string(); })));
}

bool editable_source(const Json& source) {
    if (!source.is_object() || !source.contains("schema") ||
        source.at("schema") != scenario::package_scenario_schema ||
        !source.contains("mode") || !source.at("mode").is_string() ||
        !source.contains("workloads") || !source.at("workloads").is_object() ||
        source.at("workloads").empty() || !source.contains("execution") ||
        !source.at("execution").is_object() || (source.contains("output") &&
        (!source.at("output").is_object() || (source.at("output").contains("directory") &&
          !source.at("output").at("directory").is_string())))) return false;
    const auto& execution = source.at("execution");
    if ((execution.contains("hardware_counters") && !execution.at("hardware_counters").is_boolean()) ||
        (execution.contains("controller_core") && !execution.at("controller_core").is_string())) return false;
    const auto& workloads = source.at("workloads");
    if (std::ranges::any_of(workloads, [](const auto& workload) {
        return !workload.is_object() || !workload.contains("task") || !workload.at("task").is_string() ||
               (workload.contains("parameters") && !workload.at("parameters").is_object());
    })) return false;
    if (scenario::is_package_case_group(source)) {
        return source.contains("cases") && source.at("cases").is_array() &&
            !source.at("cases").empty() && std::ranges::all_of(source.at("cases"), [&](const auto& item) {
                return item.is_object() && item.contains("id") && item.at("id").is_string() &&
                    item.contains("roles") && item.at("roles").is_array() &&
                    std::ranges::all_of(item.at("roles"), [&](const auto& role) {
                        return editable_role(role, workloads);
                    });
            });
    }
    const auto& mode = source.at("mode").get_ref<const std::string&>();
    return std::ranges::contains(modes, mode) && source.contains("roles") &&
        source.at("roles").is_array() && !source.at("roles").empty() &&
        std::ranges::all_of(source.at("roles"), [&](const auto& role) {
            return editable_role(role, workloads);
        });
}

void guided_draft(Session& s, Json source, Json execution,
                  const scenario::PackageControllerPlacement& controller) {
    execution["controller_core"] = controller.core.value();
    source["execution"] = std::move(execution);
    s.artifact.clear();
    s.draft_kind = "scenario";
    s.draft = std::move(source);
    changed(s);
}

void guide_menu(Session& s);

constexpr std::string_view matrix_view = "NUMA read bandwidth matrix";
constexpr std::string_view matrix_draft = "numa-bandwidth-matrix";

void cache_guide(Session& s, std::vector<scenario_topology::CacheSweepSite> sites, Json execution,
                 const scenario::PackageControllerPlacement& controller, core::CoreId core) {
    std::erase_if(sites, [core](const auto& site) { return site.worker->core_id == core; });
    std::vector<std::string> labels;
    for (const auto& site : sites) {
        auto detail = "CPU " + std::to_string(site.worker->linux_cpu_id.value()) + " on NUMA " +
            std::to_string(*site.local->linux_node_id.value());
        for (const auto* cache : site.caches)
            detail += "  L" + std::to_string(cache->level) + " " + std::to_string(cache->size.value()) + " B";
        labels.push_back(std::move(detail));
    }
    s.choose("Choose a cache reader", std::move(labels), [&s, sites = std::move(sites), execution, controller](
                std::size_t index) {
        const auto& site = sites.at(index);
        std::vector<std::string> nodes;
        for (const auto* node : site.remote)
            nodes.push_back("NUMA " + std::to_string(*node->linux_node_id.value()));
        s.choose("Choose remote memory", std::move(nodes), [&s, site, execution, controller](std::size_t node) {
            guided_draft(s, scenario_guides::cache_scenario(site, *site.remote.at(node)->linux_node_id.value()),
                         execution, controller);
        }, [&s, sites, execution, controller] {
            const auto core_id = *s.context().stable_ids.core(controller.core);
            cache_guide(s, sites, execution, controller, core_id);
        });
    }, [&s] { guide_menu(s); });
}

void matrix_guide(Session& s, scenario_guides::NumaMatrix matrix, Json execution,
                  const scenario::PackageControllerPlacement& controller, core::CoreId core) {
    const auto selected = scenario_guides::matrix_readers(matrix, core);
    s.page(std::string(matrix_view), [&s] { guide_menu(s); });
    for (const auto& [node, cpu] : selected)
        s.note("NUMA " + std::to_string(node), "Reader CPU " + std::to_string(cpu.value()), "Readers");
    for (const auto* node : matrix.targets)
        s.note("NUMA " + std::to_string(*node->linux_node_id.value()), "Memory", "Targets");
    const auto limits = std::to_string(matrix.minimum_kib) + " to " + std::to_string(matrix.maximum_kib) + " KiB";
    s.field_action("Set working set and create cases", [&s, matrix, execution, controller, selected, core] {
        // The ordinary Scenario is created only after the working set validates.
        s.artifact.clear();
        s.draft_kind = "scenario";
        s.draft = {{"guide", matrix_draft}, {"controller_core", controller.core.value()}};
        const auto initial = std::to_string(std::clamp<std::uint64_t>(1024U, matrix.minimum_kib,
                                                                     matrix.maximum_kib));
        s.input("Working set per reader in KiB", initial, [&s, matrix, execution, controller, selected, core](
                   std::string text) {
            const auto size = unsigned_number(text, matrix.minimum_kib, matrix.maximum_kib);
            if (!size) {
                matrix_guide(s, matrix, execution, controller, core);
                s.message = "Set the working set within the displayed limits.";
                return;
            }
            guided_draft(s, scenario_guides::numa_scenario(matrix, selected, *size), execution, controller);
        }, [&s, matrix, execution, controller, core] {
            matrix_guide(s, matrix, execution, controller, core);
        });
    }, limits, "Cases");
}

Json guide_execution(bool bandwidth) {
    return {{"repetitions", bandwidth ? 8U : 8192U}, {"blocks", 4U}};
}

void start_guide(Session& s, bool bandwidth) {
    auto guides = scenario_guides::memory_guides(*s.catalog, s.context());
    const auto execution = guide_execution(bandwidth);
    const auto choices = scenario_guides::guide_controller_choices(execution, s.context(), guides, bandwidth);
    if (choices.empty()) {
        guide_menu(s);
        s.message = "A separate controller core must leave an eligible reader on the target topology.";
        return;
    }
    select_controller(s, choices, [&s, bandwidth, choices, guides = std::move(guides), execution](std::size_t index) {
        const auto controller = choices.at(index == 0U ? 0U : index - 1U);
        const auto core = *s.context().stable_ids.core(controller.core);
        if (bandwidth) matrix_guide(s, guides.matrix, execution, controller, core);
        else cache_guide(s, guides.cache_sites, execution, controller, core);
    }, [&s] { guide_menu(s); });
}

void restore_matrix_guide(Session& s) {
    const auto stable = s.draft.value("controller_core", Json());
    if (stable.is_string()) {
        auto guides = scenario_guides::memory_guides(*s.catalog, s.context());
        const auto execution = guide_execution(true);
        const auto choices = scenario_guides::guide_controller_choices(execution, s.context(), guides, true);
        const auto controller = std::ranges::find_if(choices, [&](const auto& choice) {
            return stable == choice.core.value();
        });
        if (controller != choices.end()) {
            const auto core = *s.context().stable_ids.core(controller->core);
            matrix_guide(s, std::move(guides.matrix), execution, *controller, core);
            return;
        }
    }
    start_guide(s, true);
}

void guide_menu(Session& s) {
    s.page("Build a Scenario", [&s] { s.home(); });
    for (std::size_t i = 0; i < modes.size(); ++i) {
        s.action(std::string{mode_labels.at(i)}, [&s, i] {
            s.artifact.clear();
            s.draft_kind = "scenario";
            s.draft = {{"schema", scenario::package_scenario_schema}, {"mode", modes.at(i)},
                       {"workloads", Json::object()}, {"roles", Json::array()},
                       {"execution", {{"repetitions", 1U}, {"blocks", 1U}}}};
            add_role(s.draft);
            if (i != 0U) add_role(s.draft);
            changed(s);
        }, {}, "Execution mode");
    }
    const auto guides = scenario_guides::memory_guides(*s.catalog, s.context());
    const std::string bandwidth = "NUMA node-pair read bandwidth matrix";
    if (guides.matrix.readers.empty())
        s.note(bandwidth, "Requires an installed read package and eligible NUMA pairs", "Guides");
    else
        s.action(bandwidth, [&s] { start_guide(s, true); }, {}, "Guides");
    const std::string latency = "Dependent-load latency around cache capacities";
    if (guides.cache_sites.empty())
        s.note(latency, "Requires an installed latency package and eligible cache and NUMA placement", "Guides");
    else
        s.action(latency, [&s] { start_guide(s, false); }, {}, "Guides");
    s.action("Change target topology", [&s] { s.change_target([&s] { guide_menu(s); }); });
}

void role_panels(Session& s) {
    const auto& roles = s.draft.at("roles");
    const auto& mode = s.draft.at("mode");
    if (mode == "comparison") {
        task_slot(s, 0U, "Shared task inputs", "Task");
        task_parameters(s, roles.front().at("workload").get<std::string>(), "Shared task inputs");
    }
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const auto panel = role_label(s.draft, i);
        if (mode != "comparison") {
            task_slot(s, i, panel, "Task");
            task_parameters(s, roles.at(i).at("workload").get<std::string>(), panel);
        }
        if (!task_for_role(s, roles.at(i))) continue;
        implementation_slot(s, i, panel);
        if (implementation_for(s, roles.at(i))) {
            s.action("Placement", [&s, i] {
                placement(s, role_path(i), [&s] { show_editor(s); });
            }, placement_summary(roles.at(i)), panel);
        }
        if (mode == "pipeline") pipeline_ports(s, i, panel);
    }
    if (mode != "pipeline" && mode != "mixed") return;
    const auto unit = mode == "pipeline" ? "stage" : "role";
    if (roles.size() < 64U) {
        const auto detail = s.draft.at("execution").contains("pipeline_chunks")
            ? "Switches to staged execution" : "";
        s.action(std::string{"Add "} + unit, [&s] {
            add_role(s.draft);
            if (s.draft.at("roles").size() > 2U) s.draft.at("execution").erase("pipeline_chunks");
            changed(s);
        }, detail, "Roles");
    }
    if (roles.size() > 2U) {
        s.action(std::string{"Remove last "} + unit, [&s] {
            auto& current = s.draft.at("roles");
            const auto workload = current.back().at("workload").get<std::string>();
            current.erase(current.size() - 1U);
            if (std::ranges::none_of(current, [&](const auto& role) {
                return role.at("workload") == workload;
            })) s.draft.at("workloads").erase(workload);
            changed(s);
        }, {}, "Roles");
    }
}

} // namespace

void new_scenario(Session& s) {
    s.with_target([&s] { guide_menu(s); });
}

void edit_scenario(Session& s, bool revalidate) {
    if (!s.catalog || !s.target) {
        s.with_target([&s, revalidate] { edit_scenario(s, revalidate); });
        return;
    }
    if (s.draft.is_object() && s.draft.value("guide", Json()) == matrix_draft) {
        restore_matrix_guide(s);
        return;
    }
    s.page("Scenario authoring", [&s] { s.home(); });
    s.action("Change target topology", [&s] {
        s.change_target([&s] { edit_scenario(s, true); }, [&s] { show_editor(s); });
    }, s.topology_label, "Scenario");
    if (!editable_source(s.draft)) {
        s.note("This file cannot be edited as a package Scenario", {}, "Scenario");
        return;
    }
    s.action("Validate Scenario", [&s] { edit_scenario(s, true); }, {}, "Scenario");
    s.field_action("Save Scenario", [&s] { save_scenario(s); }, {}, "Scenario");
    if (scenario::is_package_case_group(s.draft)) {
        s.note("Comparison cases", {}, "Scenario");
        case_panel(s);
    } else {
        const auto mode = s.draft.at("mode").get<std::string>();
        const auto current = std::ranges::find(modes, mode);
        if (current != modes.end()) {
            const auto index = static_cast<std::size_t>(current - modes.begin());
            s.note(std::string{mode_labels.at(index)}, {}, "Scenario");
        }
        role_panels(s);
    }
    execution_panel(s);
    if (!scenario::is_package_case_group(s.draft)) package_catalog(s);
    if (revalidate) {
        const auto result = validate(s);
        s.message = result ? *result : result.error();
    }
}

void restore_scenario_field(Session& s, const Json& address) {
    if (address.value("view", "") == "Observation controls" && editable_source(s.draft))
        execution_options(s);
    s.reopen_field(address);
}

} // namespace epycbench::cli::tui
