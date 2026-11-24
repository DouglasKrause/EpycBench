#include "cli/tui.hpp"

#include "cli/scenario_topology.hpp"
#include "epycbench/scenario/configuration_space.hpp"
#include "kernel/package_input_policy.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace epycbench::cli::tui {
namespace {
using kernel::packages::InputPolicy;
using kernel::packages::ParameterDefinition;
using kernel::packages::ParameterType;
using kernel::packages::stream_input_placement_supported;
using scenario::ConfigurationSpace;

constexpr std::size_t sweep_limit = 64U;
constexpr std::size_t preview_limit = 12U;
constexpr std::string_view sweep_schema = scenario::package_space_schema;
constexpr std::string_view throughput = "tasks_per_second";
constexpr std::string_view time_per_task = "seconds_per_completed_task";

std::string describe(const core::Error& error) {
    return error.code.value() + " · " + error.message + " · " + error.context.dump();
}

void sweep(Session& s, bool revalidate = false);
void axis(Session& s, std::string path, Action back = {}, std::optional<std::size_t> focus = {});

Json& recipe(Session& s) {
    return s.draft.at("recipe");
}

std::string objective_label(const Json& objective) {
    return objective == throughput ? "Maximize tasks per second" : "Minimize seconds per completed task";
}

std::expected<ConfigurationSpace, core::Error> parse_recipe(Session& s) {
    const auto bytes = recipe(s).dump();
    const auto limit = scenario::is_package_case_group(recipe(s).at("scenario"))
        ? scenario::maximum_package_cases : sweep_limit;
    return ConfigurationSpace::parse({"workbench-sweep", bytes, scenario::ScenarioFormat::json}, limit);
}

struct AxisInfo {
    enum class Kind { parameter, discrete, ordered_ids, input_placement };
    Kind kind{Kind::parameter};
    std::string label, panel;
    Json base;
    std::optional<ParameterDefinition> parameter;
    std::vector<Json> choices;
};

std::vector<Json> cpu_choices(const Session& s) {
    std::vector<Json> choices;
    for (const auto* worker : scenario_topology::eligible_workers(s.target->topology))
        if (s.target->topology.machine().online_cpus.contains(worker->linux_cpu_id))
            choices.emplace_back(worker->linux_cpu_id.value());
    return choices;
}

std::vector<Json> node_choices(const Session& s) {
    std::vector<Json> choices;
    for (const auto* node : scenario_topology::eligible_nodes(s.target->topology)) {
        const auto id = *node->linux_node_id.value();
        if (s.target->topology.machine().online_nodes.contains(core::NumaNodeId{id}))
            choices.emplace_back(id);
    }
    return choices;
}

std::expected<Json, core::Error> effective_base(Session& s, const Json& base) {
    if (!scenario::is_package_case_group(base))
        return scenario::validate_package_sweep_base(base, *s.catalog, s.context());
    auto cases = scenario::expand_package_cases(base);
    if (!cases) return std::unexpected{std::move(cases.error())};
    return scenario::validate_package_sweep_base(cases->front(), *s.catalog, s.context());
}

AxisInfo axis_info(Session& s, const Json& base, const std::string& path) {
    const Json::json_pointer pointer(path);
    const auto& key = pointer.back();
    AxisInfo info;
    if (base.contains(pointer)) info.base = base.at(pointer);

    if (path.starts_with("/execution/")) {
        info.panel = "Execution";
        if (key == "hardware_counters") {
            info.kind = AxisInfo::Kind::discrete;
            info.label = "Worker hardware counters";
            info.choices = {false, true};
        } else {
            const auto upper = key == "blocks" ? 4096U : key == "repetitions" ? 1000000000U : 64U;
            info.label = key == "blocks" ? "Measured blocks" :
                         key == "repetitions" ? "Repetitions per callback" : "Streaming chunks";
            info.parameter = ParameterDefinition{ParameterType::uint64, {},
                key == "pipeline_chunks" ? Json(2U) : Json(1U), Json(upper), {}};
        }
        return info;
    }

    const auto parent = pointer.parent_pointer();
    const auto group = parent.parent_pointer();
    if (path.starts_with("/workloads/")) {
        const auto& workload = base.at(group);
        const auto task = workload.at("task").get<std::string_view>();
        const auto found = std::ranges::find_if(s.catalog->implementations(),
            [task](const auto& impl) { return impl.task->id == task; });
        info.panel = "Workload " + group.back();
        info.label = info.panel + " · " + key;
        info.parameter = found->task->parameters.at(key);
        return info;
    }

    const auto& role = base.at(group.parent_pointer() == Json::json_pointer("/roles") ? group : parent);
    info.panel = "Role " + role.at("id").get<std::string>();
    info.label = info.panel + " · " + key;
    const auto workload = role.at("workload").get<std::string_view>();
    const auto task = base.at("workloads").at(workload).at("task").get<std::string_view>();
    if (key == "implementation" && parent.parent_pointer() == Json::json_pointer("/roles")) {
        info.kind = AxisInfo::Kind::discrete;
        for (const auto& impl : s.catalog->implementations())
            if (impl.task->id == task) info.choices.emplace_back(impl.id);
    } else if (parent.back() == "tuning") {
        const auto* implementation = s.catalog->find(task, role.at("implementation").get<std::string_view>());
        info.parameter = implementation->tuning.at(key);
    } else if (key == "input_placement") {
        info.kind = AxisInfo::Kind::input_placement;
        info.choices = node_choices(s);
    } else {
        info.kind = (key == "cpus" || key == "numa_nodes")
            ? AxisInfo::Kind::ordered_ids : AxisInfo::Kind::discrete;
        info.choices = (key == "cpus" || key == "service_cpu") ? cpu_choices(s) : node_choices(s);
    }
    return info;
}

std::string setting_type(const ParameterDefinition& spec) {
    using enum ParameterType;
    switch (spec.type) {
    case uint64: return "Unsigned integer";
    case int64: return "Signed integer";
    case float64: return "Number";
    case boolean: return "Boolean";
    case enumeration: return "Manifest choices";
    }
    return {};
}

Json& axes(Session& s) {
    return recipe(s).at("axes");
}

auto find_axis(Session& s, const std::string& path) {
    return std::ranges::find_if(axes(s), [&](const Json& item) { return item.at("path") == path; });
}

auto pending_key(const std::string& path, std::size_t index) -> std::string {
    return path + "#" + std::to_string(index);
}

Json choices(Session& s, const std::string& path) {
    const auto found = find_axis(s, path);
    return found == axes(s).end() ? Json::array() : found->at("values");
}

void set_choice(Session& s, const std::string& path, std::size_t index, Json value, const Action& refresh) {
    auto found = find_axis(s, path);
    if (found == axes(s).end()) {
        axes(s).push_back({{"path", path}, {"values", Json::array()}});
        found = std::prev(axes(s).end());
    }
    auto& values = (*found)["values"];
    if (index < values.size()) values.at(index) = std::move(value);
    else values.push_back(std::move(value));
    s.draft.at("pending").erase(pending_key(path, index));
    refresh();
    s.persist();
}

void forget_pending(Session& s, const std::string& path) {
    auto& pending = s.draft.at("pending");
    for (auto it = pending.begin(); it != pending.end();) {
        if (it.key().starts_with(path + "#")) it = pending.erase(it);
        else ++it;
    }
}

std::vector<std::string> labels_for(const std::vector<Json>& options) {
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& option : options)
        labels.push_back(option.is_string() ? option.get<std::string>() : option.dump());
    return labels;
}

using SelectJson = std::function<void(Json)>;

void pick_json(Session& s, std::string heading, std::vector<Json> values, SelectJson select, Action cancel) {
    auto labels = labels_for(values);
    s.choose(std::move(heading), std::move(labels),
        [values = std::move(values), select = std::move(select)](std::size_t index) {
            select(values.at(index));
        }, std::move(cancel));
}

std::string short_json(const Json& value, std::size_t maximum = 140U) {
    auto text = value.dump();
    if (text.size() > maximum) text = text.substr(0, maximum) + " …";
    return text;
}

void typed(Session& s, const std::string& path, std::size_t index,
           std::string heading, std::string initial,
           std::function<std::expected<Json, std::string>(std::string_view)> parse, Action refresh,
           std::string policy = {}) {
    const auto key = pending_key(path, index);
    auto& pending = s.draft.at("pending");
    if (pending.contains(key)) initial = pending.at(key).at("text").get<std::string>();
    s.input(std::move(heading), std::move(initial),
        [&s, path, index, key, parse = std::move(parse), refresh](std::string text) {
            auto value = parse(text);
            if (value) {
                set_choice(s, path, index, std::move(*value), refresh);
                return;
            }
            s.draft.at("pending")[key] = {{"text", text}, {"error", value.error()}};
            refresh();
            s.message = value.error();
            s.persist();
        }, refresh, {{"field", "choice"}, {"path", path}, {"index", index}, {"policy", std::move(policy)}});
}

constexpr std::string_view id_list_prompt = "Enter an ordered, comma-separated list of 1 to 1024 IDs.";
constexpr std::string_view stream_node_prompt = "Enter one eligible NUMA node ID.";

std::expected<Json, std::string> parse_ids(std::string_view text, const std::vector<Json>& eligible,
                                          std::string_view invalid_list = id_list_prompt) {
    auto input = std::string(text);
    if (!text.starts_with("[")) input = "[" + input + "]";
    auto list = Json::parse(input, nullptr, false);
    if (!list.is_array() || list.empty() || list.size() > 1024U)
        return std::unexpected{std::string{invalid_list}};
    for (const auto& id : list) {
        if (!id.is_number_unsigned() || !std::ranges::contains(eligible, id))
            return std::unexpected{"Each ID must be one of the eligible indices shown for this axis."};
    }
    return list;
}

void edit_nodes(Session& s, const std::string& path, std::size_t index, const AxisInfo& info,
                InputPolicy policy, const Json& initial, Action refresh) {
    const auto eligible = info.choices;
    const bool streaming = scenario::has_stream_chunks(recipe(s).at("scenario"));
    const std::string name{kernel::packages::input_policy_name(policy)};
    std::string prompt = (streaming ? "One NUMA node ID for " : "NUMA node IDs for ") + name +
                         " · eligible " + short_json(eligible);
    typed(s, path, index, std::move(prompt), initial.is_array() ? initial.dump() : "",
        [eligible, policy, name, streaming](std::string_view text) -> std::expected<Json, std::string> {
            auto nodes = parse_ids(text, eligible, streaming ? stream_node_prompt : id_list_prompt);
            if (!nodes) return nodes;
            if (streaming && !stream_input_placement_supported(policy, nodes->size()))
                return std::unexpected{"Streaming supports one explicit input memory node. Enter one eligible node ID."};
            if (policy == InputPolicy::interleave && nodes->size() < 2U)
                return std::unexpected{"Interleave requires at least two NUMA nodes."};
            return Json{{"policy", name}, {"nodes", std::move(*nodes)}};
        }, std::move(refresh), name);
}

void edit_replica(Session& s, const std::string& path, std::size_t index, const Json& initial,
                  InputPolicy policy, Action refresh) {
    const auto group = initial.value("replica_group", Json());
    const std::string name{kernel::packages::input_policy_name(policy)};
    typed(s, path, index, "Replica group identifier", group.is_string() ? group.get<std::string>() : "",
        [name](std::string_view text) -> std::expected<Json, std::string> {
            if (text.empty()) return std::unexpected{"Enter a replica group identifier."};
            return Json{{"policy", name}, {"replica_group", text}};
        }, std::move(refresh), name);
}

void edit_input_placement(Session& s, const std::string& path, std::size_t index,
                          AxisInfo info, Json initial, Action refresh) {
    struct Option {
        InputPolicy policy;
        std::string_view label;
        std::size_t minimum_nodes{};
    };
    constexpr std::array placement_options{
        Option{InputPolicy::bind, "Bind to worker memory"},
        Option{InputPolicy::bind, "Bind to named nodes", 1U},
        Option{InputPolicy::interleave, "Interleave across nodes", 2U},
        Option{InputPolicy::replica_per_worker, "Read-only replica per worker"},
        Option{InputPolicy::replica_per_node, "Read-only replica per node"}};
    const bool streaming = scenario::has_stream_chunks(recipe(s).at("scenario"));
    std::vector<std::string> labels;
    std::vector<Option> options;
    for (const auto& option : placement_options) {
        if (streaming && !stream_input_placement_supported(option.policy, option.minimum_nodes)) continue;
        labels.emplace_back(streaming && option.minimum_nodes != 0U ? "Bind to one NUMA node" : option.label);
        options.push_back(option);
    }
    s.choose("Input placement policy", std::move(labels),
        [&s, path, index, info = std::move(info), initial = std::move(initial),
         options = std::move(options), refresh](std::size_t selection) {
            const auto& option = options.at(selection);
            if (option.minimum_nodes != 0U) {
                edit_nodes(s, path, index, info, option.policy, initial.value("nodes", Json()), refresh);
            } else if (option.policy == InputPolicy::bind) {
                set_choice(s, path, index, {{"policy", "bind"}}, refresh);
            } else {
                edit_replica(s, path, index, initial, option.policy, refresh);
            }
        }, refresh);
}

void edit_choice(Session& s, const std::string& path, std::size_t index, const AxisInfo& info,
                 Action refresh) {
    auto values = choices(s, path);
    auto initial = index < values.size() ? values.at(index) : info.base;
    auto select = [&s, path, index, refresh](Json value) { set_choice(s, path, index, std::move(value), refresh); };
    using enum AxisInfo::Kind;
    if (info.kind == discrete) {
        pick_json(s, info.label, info.choices, select, refresh);
    } else if (info.kind == input_placement) {
        edit_input_placement(s, path, index, info, initial.is_object() ? initial : Json::object(), refresh);
    } else if (info.kind == ordered_ids) {
        const auto eligible = info.choices;
        typed(s, path, index, info.label + " · eligible " + short_json(eligible),
            initial.is_null() ? "" : initial.dump(),
            [eligible](std::string_view text) { return parse_ids(text, eligible); }, refresh);
    } else {
        const auto& spec = *info.parameter;
        if (spec.type == ParameterType::boolean) {
            pick_json(s, info.label, {false, true}, select, refresh);
        } else if (spec.type == ParameterType::enumeration) {
            pick_json(s, info.label, std::vector<Json>(spec.values.begin(), spec.values.end()), select, refresh);
        } else {
            auto name = setting_type(spec);
            if (spec.minimum) name += " · minimum " + spec.minimum->dump();
            if (spec.maximum) name += " · maximum " + spec.maximum->dump();
            typed(s, path, index, info.label + " · " + name,
                initial.is_null() ? "" : initial.dump(),
                [spec, path](std::string_view text) -> std::expected<Json, std::string> {
                    auto value = Json::parse(text, nullptr, false);
                    if (value.is_discarded()) return std::unexpected{"Enter " + setting_type(spec) + "."};
                    const kernel::packages::ParameterDefinitions definitions{{"value", spec}};
                    auto checked = kernel::packages::resolve_parameters(definitions, {{"value", value}}, path);
                    if (!checked) return std::unexpected{describe(checked.error())};
                    return std::move(checked->at("value"));
                }, refresh);
        }
    }
}

void axis(Session& s, std::string path, Action back, std::optional<std::size_t> focus) {
    if (!back) back = [&s] { sweep(s); };
    const auto refresh = [&s, path, back, focus] { axis(s, path, back, focus); };
    auto effective = effective_base(s, recipe(s).at("scenario"));
    if (!effective) {
        back();
        s.message = describe(effective.error());
        return;
    }
    const auto info = axis_info(s, *effective, path);
    s.page(info.label, back);
    s.note("Saved value  " + short_json(info.base), path);
    auto values = choices(s, path);
    const auto& pending = s.draft.at("pending");
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto key = pending_key(path, index);
        const auto has_pending = pending.contains(key);
        const auto detail = has_pending ? pending.at(key).at("error").get<std::string>() : values.at(index).dump();
        if (focus == index) s.selected = static_cast<int>(s.rows.size());
        s.action("Edit choice " + std::to_string(index + 1) + "  " + short_json(values.at(index)),
            [&s, path, index, info, refresh] { edit_choice(s, path, index, info, refresh); }, detail);
        s.action("Remove choice " + std::to_string(index + 1), [&s, path, index, refresh] {
            auto& list = (*find_axis(s, path))["values"];
            list.erase(list.begin() + static_cast<Json::difference_type>(index));
            const auto remaining = list.size();
            if (remaining == 0U) axes(s).erase(find_axis(s, path));
            auto& unfinished = s.draft.at("pending");
            for (std::size_t i = index; i <= remaining; ++i) {
                unfinished.erase(pending_key(path, i));
                const auto next = unfinished.find(pending_key(path, i + 1U));
                if (next != unfinished.end()) {
                    auto retained = std::move(*next);
                    unfinished.erase(next);
                    unfinished[pending_key(path, i)] = std::move(retained);
                }
            }
            refresh();
            s.persist();
        });
    }
    const auto key = pending_key(path, values.size());
    if (pending.contains(key)) {
        s.note("Unfinished choice  " + pending.at(key).at("text").get<std::string>(),
            pending.at(key).at("error").get<std::string>());
        s.action("Discard unfinished choice", [&s, key, refresh] {
            s.draft.at("pending").erase(key);
            refresh();
            s.persist();
        });
    }
    if (focus == values.size()) s.selected = static_cast<int>(s.rows.size());
    s.action("Add choice", [&s, path, index = values.size(), info, refresh] {
        edit_choice(s, path, index, info, refresh);
    });
    if (!values.empty() || pending.contains(key)) {
        s.action("Remove axis", [&s, path, back] {
            const auto found = find_axis(s, path);
            if (found != axes(s).end()) axes(s).erase(found);
            forget_pending(s, path);
            back();
            s.persist();
        });
    }
}

std::vector<std::string> candidate_errors(const scenario::ConfigurationCandidate& candidate) {
    std::vector<std::string> lines;
    for (const auto& error : candidate.diagnostics)
        lines.push_back(error.code.value() + " · " + error.json_pointer + " · " + Json(error.safe_context).dump());
    return lines;
}

struct Validation {
    std::size_t accepted{}, rejected{};
    std::vector<std::pair<std::size_t, std::string>> examples;
};

Validation validate_candidates(Session& s, const ConfigurationSpace& space) {
    Validation validation;
    for (std::size_t index = 0; index < space.size(); ++index) {
        auto candidate = space.resolve(index, s.context());
        if (candidate && candidate->accepted()) {
            ++validation.accepted;
            continue;
        }
        ++validation.rejected;
        if (validation.examples.size() < preview_limit) {
            std::string line;
            if (candidate) {
                auto details = candidate_errors(*candidate);
                line += details.empty() ? "Rejected" : details.front();
            } else {
                line += describe(candidate.error());
            }
            validation.examples.emplace_back(index, std::move(line));
        }
    }
    return validation;
}

void show_candidate(Session& s, std::size_t ordinal, Action back);

void preview(Session& s, std::size_t offset) {
    auto space = parse_recipe(s);
    if (!space) {
        sweep(s);
        s.message = describe(space.error());
        return;
    }
    offset = std::min(offset, (space->size() - 1U) / preview_limit * preview_limit);
    s.page("Candidate preview", [&s] { sweep(s); });
    const auto end = std::min(offset + preview_limit, space->size());
    s.note(std::to_string(space->size()) + " candidates",
        objective_label(recipe(s).at("objective")));
    s.note("Ordinals " + std::to_string(offset) + "–" + std::to_string(end - 1U));
    if (!s.draft.at("pending").empty())
        s.note("Preview uses applied choices", "Finish or discard the unfinished choices before saving.");
    if (offset != 0U)
        s.action("Previous page", [&s, offset] { preview(s, offset - preview_limit); });
    if (end < space->size())
        s.action("Next page", [&s, end] { preview(s, end); });
    for (auto index = offset; index < end; ++index) {
        auto candidate = space->resolve(index, s.context());
        const auto label = "Candidate " + std::to_string(index);
        const bool accepted = candidate && candidate->accepted();
        auto detail = accepted ? candidate->candidate_identity->to_hex() : std::string{"View diagnostics"};
        const auto status = accepted ? " · Resolved" : " · Rejected";
        s.action(label + status, [&s, index, offset] {
            show_candidate(s, index, [&s, offset] { preview(s, offset); });
        }, std::move(detail), "Candidates");
    }
}

void candidate_settings(Session& s, const Json* authored, const std::vector<std::string>& related,
                        Action back, bool recipe_wide) {
    if (related.empty()) return;
    const auto& base = recipe(s).at("scenario");
    auto eligible = scenario::package_axes(base, *s.catalog);
    auto effective = effective_base(s, base);
    std::vector<std::string> labels;
    std::vector<Action> edits;
    std::set<std::string> seen;
    for (const auto& path : related) {
        if (!seen.insert(path).second) continue;
        const auto options = choices(s, path);
        if (!eligible || !effective || !std::ranges::contains(*eligible, path)) {
            if (!options.empty() && eligible && !std::ranges::contains(*eligible, path)) {
                labels.push_back("Remove unavailable axis " + path);
                edits.push_back([&s, path, back] {
                    axes(s).erase(find_axis(s, path));
                    forget_pending(s, path);
                    back();
                    s.persist();
                });
            }
            continue;
        }
        const auto info = axis_info(s, *effective, path);
        const auto pointer = Json::json_pointer(path);
        std::optional<std::size_t> focus;
        auto label = "Edit axis · " + info.label;
        if (options.empty()) {
            label = "Add axis · " + info.label;
            focus = 0U;
        }
        else if (!recipe_wide && authored && authored->contains(pointer)) {
            const auto selected = std::ranges::find(options, authored->at(pointer));
            if (selected != options.end()) {
                focus = static_cast<std::size_t>(selected - options.begin());
                label += "  " + short_json(*selected, 48U);
            }
        }
        labels.push_back(std::move(label));
        edits.push_back([&s, path, back, focus] { axis(s, path, back, focus); });
    }
    if (edits.size() == 1U) s.action(std::move(labels.front()), std::move(edits.front()), {}, "Settings");
    if (edits.size() > 1U)
        s.action("Choose a related setting", [&s, labels = std::move(labels), edits = std::move(edits), back] {
            s.choose("Related sweep settings", labels, [edits](std::size_t index) { edits.at(index)(); }, back);
        }, {}, "Settings");
}

void show_candidate(Session& s, std::size_t ordinal, Action back) {
    auto space = parse_recipe(s);
    if (!space || ordinal >= space->size()) {
        back();
        if (!space) s.message = describe(space.error());
        return;
    }
    std::vector<std::string> related;
    const auto candidate = space->resolve(ordinal, s.context(), &related);
    const auto authored = space->materialize(ordinal);
    s.page("Candidate " + std::to_string(ordinal), back);
    if (candidate) {
        for (const auto& error : candidate_errors(*candidate)) s.note("Rejected", error);
        if (related.empty())
            for (const auto& error : candidate->diagnostics)
                if (!error.json_pointer.empty()) related.push_back(error.json_pointer);
    } else {
        s.note("Rejected", describe(candidate.error()));
    }
    if (!candidate || !candidate->accepted()) {
        const auto revisit = [&s, ordinal, back, cardinalities = space->axis_cardinalities(), authored] {
            const auto revised = parse_recipe(s);
            if (revised && revised->axis_cardinalities() != cardinalities &&
                revised->materialize(ordinal) != authored) {
                back();
                return;
            }
            show_candidate(s, ordinal, back);
        };
        const bool recipe_wide = !candidate &&
            candidate.error().context.value("path", std::string{}) == "/execution/controller_core";
        const std::optional<Json> settings = authored ? std::optional<Json>(*authored) : std::nullopt;
        candidate_settings(s, settings ? &*settings : nullptr, related, revisit, recipe_wide);
    }
    for (const auto& definition : axes(s)) {
        const auto path = definition.at("path").get<std::string>();
        const auto pointer = nlohmann::ordered_json::json_pointer(path);
        if (authored && authored->contains(pointer))
            s.note(path, authored->at(pointer).dump(), "Axis choices");
    }
    if (authored && authored->contains("case_id"))
        s.note("Comparison case", authored->at("case_id").get<std::string>());
    if (!candidate || !candidate->accepted()) return;
    const auto& plan = *candidate->package;
    s.note("Candidate identity", plan.candidate_identity.to_hex(), "Identity");
    s.note("Problem identity", plan.problem_identity.to_hex(), "Identity");
    s.note("Input identity", plan.input_identity.to_hex(), "Identity");
    s.note("Resolved plan digest", plan.resolved_digest.to_hex(), "Identity");
    if (plan.controller)
        s.note("Controller CPU " + std::to_string(plan.controller->cpu.value()),
            std::string(plan.controller->core.value()), "Placement");
    for (const auto& role : plan.evidence.at("scenario").at("roles")) {
        s.note("Role " + role.at("id").get<std::string>(),
            "CPUs " + role.at("cpus").dump() + " · NUMA " +
                role.value("numa_nodes", role.value("numa_node", Json())).dump(), "Placement");
    }
}

void review_save(Session& s, bool publish) {
    if (!s.draft.at("pending").empty()) {
        s.message = "Finish or discard the unfinished choices before validating the recipe.";
        return;
    }
    auto space = parse_recipe(s);
    if (!space) {
        s.message = describe(space.error());
        return;
    }
    auto result = validate_candidates(s, *space);
    s.page("Sweep validation", [&s] { sweep(s); });
    s.note(std::to_string(result.accepted) + " resolved · " + std::to_string(result.rejected) +
        " rejected · " + std::to_string(space->size()) + " total");
    for (const auto& [ordinal, line] : result.examples)
        s.action("Candidate " + std::to_string(ordinal) + " · Rejected", [&s, ordinal] {
            show_candidate(s, ordinal, [&s] {
                if (!s.draft.at("pending").empty()) sweep(s);
                else review_save(s, false);
            });
        }, line, "Rejected candidates");
    if (result.rejected != 0U) return;
    auto save_recipe = [&s] {
        auto location = s.draft.value("destination", std::string{});
        if (location.empty() && !s.scenario_dir.empty()) location = (s.scenario_dir / "sweep.json").string();
        s.input("New recipe filename", std::move(location), [&s](std::string filename) {
            s.draft["destination"] = filename;
            s.persist();
            Path destination{filename};
            auto failure = std::string{};
            if (destination.empty() || !destination.has_filename() ||
                destination.filename() == "." || destination.filename() == "..")
                failure = "Enter a new filename for the recipe.";
            if (failure.empty() && !destination.has_extension()) destination += ".json";
            if (failure.empty() && destination.extension() != ".json" && destination.extension() != ".yaml" &&
                destination.extension() != ".yml")
                failure = "Use a .json, .yaml or .yml recipe filename.";
            if (!failure.empty()) {
                review_save(s, false);
                s.message = std::move(failure);
                return;
            }
            if (!destination.has_parent_path() && !s.scenario_dir.empty())
                destination = s.scenario_dir / destination;
            if (!s.save(destination, recipe(s))) {
                auto message = std::move(s.message);
                review_save(s, false);
                s.message = std::move(message);
                return;
            }
            s.artifact = destination;
            s.clear_draft();
            s.page("Saved sweep recipe", [&s] { s.home(); });
            s.note(destination.string());
            s.action("Plan experiment", [&s, destination] { open_plan(s, destination); });
        }, [&s] { review_save(s, false); }, {{"field", "destination"}});
    };
    if (publish) save_recipe();
    else s.action("Save new recipe", std::move(save_recipe));
}

void inspect_base(Session& s) {
    const auto& base = recipe(s).at("scenario");
    auto effective = effective_base(s, base);
    auto eligible = scenario::package_axes(base, *s.catalog);
    s.page("Saved Scenario", [&s] { sweep(s); });
    s.note("Execution mode  " + base.at("mode").get<std::string>());
    if (!effective || !eligible) {
        s.message = describe(effective ? eligible.error() : effective.error());
        return;
    }
    std::map<std::string, std::vector<std::pair<std::string, AxisInfo>>> by_group;
    for (const auto& path : *eligible) {
        auto info = axis_info(s, *effective, path);
        by_group[info.panel].emplace_back(path, std::move(info));
    }
    const auto setting_rows = [&s, &by_group](const std::string& panel) {
        const auto group = by_group.find(panel);
        if (group == by_group.end()) return;
        for (const auto& [path, info] : group->second) {
            const auto values = choices(s, path);
            const auto prefix = values.empty() ? "Add to sweep · " : "Edit sweep axis · ";
            const auto display = info.label.starts_with(panel + " · ")
                ? info.label.substr(panel.size() + std::string_view(" · ").size()) : info.label;
            s.action(prefix + display, [&s, path] { axis(s, path); },
                "Saved value " + short_json(info.base), panel);
        }
    };
    for (auto workload = base.at("workloads").begin(); workload != base.at("workloads").end(); ++workload) {
        const auto panel = "Workload " + workload.key();
        s.note(workload->at("task").get<std::string>(), workload->value("parameters", Json::object()).dump(), panel);
        setting_rows(panel);
    }
    if (scenario::is_package_case_group(base)) {
        for (const auto& one : base.at("cases"))
            for (const auto& role : one.at("roles"))
                s.note(role.at("id").get<std::string>(), role.dump(), one.at("id").get<std::string>());
    } else {
        for (const auto& role : effective->at("roles")) {
            const auto panel = "Role " + role.at("id").get<std::string>();
            s.note(role.at("implementation").get<std::string>(), role.dump(), panel);
            setting_rows(panel);
        }
    }
    s.note("Saved observation controls", base.at("execution").dump(), "Execution");
    setting_rows("Execution");
}

void sweep(Session& s, bool revalidate) {
    s.page("Sweep recipe", [&s] { s.home(); });
    s.note(s.draft.value("source", std::string{"Embedded Scenario"}), "Read-only Scenario", "Source");
    s.action("Change target topology", [&s] { s.change_target([&s] { sweep(s, true); }, [&s] { sweep(s); }); },
        s.topology_label, "Target");
    const auto& base = recipe(s).at("scenario");
    auto effective = effective_base(s, base);
    if (!effective) {
        s.message = describe(effective.error());
        return;
    }
    auto eligible = scenario::package_axes(base, *s.catalog);
    if (!eligible) {
        s.message = describe(eligible.error());
        return;
    }
    s.action("Inspect saved Scenario", [&s] { inspect_base(s); },
        base.at("mode").get<std::string>(), "Source");
    s.action("Objective", [&s] {
        s.choose("Sweep objective", {"Maximize tasks per second", "Minimize seconds per completed task"},
            [&s](std::size_t choice) {
                recipe(s)["objective"] = choice == 0U ? throughput : time_per_task;
                sweep(s);
                s.persist();
            }, [&s] { sweep(s); });
    }, objective_label(recipe(s).at("objective")));
    for (const auto& definition : axes(s)) {
        const auto path = definition.at("path").get<std::string>();
        const auto is_eligible = std::ranges::contains(*eligible, path);
        if (is_eligible) {
            const auto info = axis_info(s, *effective, path);
            s.action(info.label, [&s, path] { axis(s, path); }, short_json(definition.at("values")), "Axes");
        } else {
            s.action("Remove unavailable axis " + path, [&s, path] {
                axes(s).erase(find_axis(s, path));
                forget_pending(s, path);
                sweep(s);
                s.persist();
            }, "The selected topology and installed modules do not admit this axis.", "Axes");
        }
    }
    for (const auto& path : *eligible) {
        const auto key = pending_key(path, 0U);
        if (find_axis(s, path) == axes(s).end() && s.draft.at("pending").contains(key))
            s.action("Resume unfinished axis", [&s, path] { axis(s, path); }, path, "Axes");
    }
    s.action("Add or edit axis", [&s, available = std::move(*eligible), base = std::move(*effective)] {
        std::vector<std::string> labels;
        for (const auto& path : available) {
            const auto info = axis_info(s, base, path);
            labels.push_back(info.label + " · saved " + short_json(info.base, 48U));
        }
        s.choose("Eligible sweep axes", std::move(labels),
            [&s, available](std::size_t selection) { axis(s, available.at(selection)); },
            [&s] { sweep(s); });
    }, {}, "Axes");
    auto parsed = parse_recipe(s);
    s.action("Preview candidates", [&s] { preview(s, 0U); },
        parsed ? std::to_string(parsed->size()) + " candidates" : describe(parsed.error()), "Review");
    s.action("Validate all candidates", [&s] { review_save(s, false); }, {}, "Review");
    s.action("Save recipe", [&s] { review_save(s, true); }, {}, "Review");
    if (revalidate) {
        if (!parsed) {
            s.message = describe(parsed.error());
            return;
        }
        const auto checked = validate_candidates(s, *parsed);
        s.message = checked.examples.empty()
            ? "Resolved all " + std::to_string(checked.accepted) + " candidates on the target topology."
            : std::to_string(checked.rejected) + " rejected · Candidate " +
                std::to_string(checked.examples.front().first) + " · " + checked.examples.front().second;
    }
}
} // namespace

void open_sweep(Session& s, Path source) {
    auto document = s.load(source);
    if (!document || !document->is_object() ||
        document->value("schema", Json()) != scenario::package_scenario_schema) {
        s.message = document ? "Choose a saved package Scenario." : document.error();
        return;
    }
    s.with_target([&s, source = std::move(source), base = std::move(*document)]() mutable {
        s.artifact = source;
        s.draft_kind = "sweep";
        s.draft = {{"source", source.string()}, {"pending", Json::object()},
            {"recipe", {{"schema", sweep_schema}, {"objective", throughput},
                {"scenario", std::move(base)}, {"axes", Json::array()}}}};
        sweep(s);
        s.persist();
    });
}

void edit_sweep(Session& s) {
    s.with_target([&s] {
        const bool importing = s.draft.is_object() && s.draft.value("schema", Json()) == sweep_schema;
        if (importing) {
            // Import a saved ConfigurationSpace while keeping the original file create-only.
            auto bytes = s.draft.dump();
            auto parsed = ConfigurationSpace::parse(
                {"workbench-sweep", bytes, scenario::ScenarioFormat::json}, scenario::maximum_package_cases);
            if (!parsed) {
                s.home();
                s.message = describe(parsed.error());
                return;
            }
            auto definition = std::move(s.draft);
            s.draft = {{"recipe", std::move(definition)}, {"pending", Json::object()}};
        }
        if (!s.draft.is_object() || !s.draft.contains("recipe") || !s.draft.contains("pending")) {
            s.home();
            s.message = "The sweep draft is unavailable.";
            return;
        }
        sweep(s, true);
        if (importing) s.persist();
    });
}

void restore_sweep_field(Session& s, const Json& address) {
    if (!address.is_object() || !s.target || !s.catalog || !s.draft.contains("recipe") ||
        !s.draft.contains("pending"))
        return;
    const auto& document = recipe(s);
    if (!document.contains("scenario") || !document.contains("axes") || !document.at("axes").is_array())
        return;
    const auto field = address.value("field", Json());
    if (field == "destination") {
        review_save(s, true);
        return;
    }
    const auto path_value = address.value("path", Json());
    const auto index_value = address.value("index", Json());
    const auto policy_value = address.value("policy", Json());
    if (field != "choice" || !path_value.is_string() || !index_value.is_number_unsigned() ||
        !policy_value.is_string())
        return;
    const auto path = path_value.get<std::string>();
    const auto index = index_value.get<std::uint64_t>();
    const auto policy = policy_value.get<std::string>();
    const auto& base = document.at("scenario");
    const auto eligible = scenario::package_axes(base, *s.catalog);
    const auto effective = effective_base(s, base);
    if (!eligible || !effective || !std::ranges::contains(*eligible, path)) return;
    const auto values = choices(s, path);
    if (index > values.size()) return;
    const auto info = axis_info(s, *effective, path);
    const auto refresh = [&s, path, choice = static_cast<std::size_t>(index)] {
        axis(s, path, {}, choice);
    };
    if (info.kind == AxisInfo::Kind::input_placement) {
        const auto restored = kernel::packages::parse_input_policy(policy);
        if (!restored) return;
        auto initial = index < values.size() ? values.at(index) : info.base;
        if (!initial.is_object()) initial = Json::object();
        if (scenario::has_stream_chunks(base) && !stream_input_placement_supported(*restored, 0U)) {
            edit_input_placement(s, path, index, info, std::move(initial), refresh);
            s.message = "Streaming input memory uses bind. Choose a bind option.";
        } else if (*restored == InputPolicy::bind || *restored == InputPolicy::interleave) {
            edit_nodes(s, path, index, info, *restored, initial.value("nodes", Json()), refresh);
        } else {
            edit_replica(s, path, index, initial, *restored, refresh);
        }
        return;
    }
    if (policy.empty() && (info.kind == AxisInfo::Kind::ordered_ids ||
        (info.parameter && info.parameter->type != ParameterType::boolean &&
         info.parameter->type != ParameterType::enumeration)))
        edit_choice(s, path, index, info, refresh);
}

} // namespace epycbench::cli::tui
