#include "cli/tui_placement.hpp"

#include "epycbench/scenario/identifier.hpp"
#include "epycbench/topology/worker_locality.hpp"
#include "kernel/package_input_policy.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace epycbench::cli::tui {
namespace {
using scenario::ResourceResolutionContext;
using kernel::packages::InputPolicy;
using kernel::packages::MappingResource;
using kernel::packages::TaskManifest;
using kernel::packages::stream_input_placement_supported;
using Group = TopologyGroupKind;
using Target = PlacementTarget;
using Cpu = core::LinuxCpuId;
using Unit = topology::ProcessingUnit;

auto lookup(const Json& source, std::string_view pointer) -> const Json* {
    const Json::json_pointer path{std::string{pointer}};
    return source.contains(path) ? &source.at(path) : nullptr;
}

auto value_or(const Json& source, std::string_view pointer, Json fallback) -> Json {
    if (const auto* value = lookup(source, pointer)) return *value;
    return fallback;
}

auto input_policy(const Json& value) -> std::optional<InputPolicy> {
    if (!value.is_string()) return std::nullopt;
    return kernel::packages::parse_input_policy(value.get_ref<const std::string&>());
}

auto accepts_input_nodes(std::optional<InputPolicy> policy, bool streaming) -> bool {
    if (!policy) return false;
    if (streaming) return stream_input_placement_supported(*policy, 0U);
    return policy == InputPolicy::bind || policy == InputPolicy::interleave;
}

auto task_for(const Json& source, const Json& role, const ResourceResolutionContext& context)
    -> const TaskManifest* {
    if (!context.packages || !role.contains("workload") || !role.at("workload").is_string() ||
        !role.contains("implementation") || !role.at("implementation").is_string()) return nullptr;
    const auto workload_pointer = Json::json_pointer{"/workloads"} / role.at("workload").get<std::string>() / "task";
    if (!source.contains(workload_pointer) || !source.at(workload_pointer).is_string()) return nullptr;
    const auto* implementation = context.packages->find(source.at(workload_pointer).get_ref<const std::string&>(),
                                                       role.at("implementation").get_ref<const std::string&>());
    return implementation ? implementation->task.get() : nullptr;
}

auto mapping_kind(const Json& source, const Json& role, const ResourceResolutionContext& context)
    -> MappingResource {
    if (const auto* task = task_for(source, role, context)) return task->mapping;
    return MappingResource::none;
}

auto is_online(const topology::Machine& machine, Cpu cpu) -> bool {
    const auto& effective = machine.effective_cpus.value();
    return effective && effective->contains(cpu) && machine.online_cpus.contains(cpu);
}

auto usable_node(const ResourceResolutionContext& context, std::uint32_t id) -> bool {
    const auto& machine = context.topology.machine();
    return machine.effective_nodes.value() &&
           machine.effective_nodes.value()->contains(core::NumaNodeId{id}) &&
           machine.online_nodes.contains(core::NumaNodeId{id}) &&
           std::ranges::any_of(context.topology.numa_nodes(), [id](const auto& node) {
               return node.linux_node_id.value() == id && node.installed_memory.value() &&
                      node.installed_memory.value()->value() > 0;
           });
}

auto find_cpu(const ResourceResolutionContext& context, Cpu id) -> const Unit* {
    const auto& units = context.topology.processing_units();
    const auto found = std::ranges::find(units, id, &Unit::linux_cpu_id);
    return found == units.end() ? nullptr : &*found;
}

auto linux_cpu(const Json& value) -> std::optional<Cpu> {
    if (!value.is_number_unsigned() || value > UINT32_MAX) return std::nullopt;
    return Cpu{value.get<std::uint32_t>()};
}

auto cpu_core(const ResourceResolutionContext& context, const Json& cpu) -> std::optional<core::CoreId> {
    if (const auto id = linux_cpu(cpu)) {
        if (const auto* unit = find_cpu(context, *id)) return unit->core_id;
    }
    return std::nullopt;
}

auto controller_core(const Json& source, const ResourceResolutionContext& context) -> std::optional<core::CoreId> {
    const auto* setting = lookup(source, "/execution/controller_core");
    if (!setting || !setting->is_string()) return {};
    const auto stable = topology::StableCoreId::parse(setting->get_ref<const std::string&>());
    return stable ? context.stable_ids.core(*stable) : std::nullopt;
}

auto local_memory(const ResourceResolutionContext& context, const Unit& worker)
    -> std::optional<topology::LocalMemoryNode> {
    const auto& page_sizes = context.topology.machine().page_sizes;
    if (page_sizes.empty()) return std::nullopt;
    return topology::unique_local_memory_node(context.topology, context.stable_ids,
                                              worker.id, std::ranges::min(page_sizes));
}

auto first_touch_mode(const Json& source, const Json& role, const TaskManifest& task) -> Json {
    const auto path = Json::json_pointer{"/workloads"} / role.at("workload").get<std::string>() /
                      "parameters" / "page_mode";
    if (source.contains(path)) return source.at(path);
    const auto definition = task.parameters.find("page_mode");
    return definition != task.parameters.end() ? definition->second.default_value.value_or(nullptr)
                                               : Json(nullptr);
}

auto first_touch_memory(const ResourceResolutionContext& context, const Unit& worker, const Json& mode)
    -> std::expected<topology::LocalMemoryNode, std::string> {
    const auto& machine = context.topology.machine();
    const auto base = machine.page_sizes.empty() ? core::ByteCount::from_base_units(0U)
                                                 : std::ranges::min(machine.page_sizes);
    const auto page = scenario::first_touch_page_size(machine, mode, base);
    if (!page || !page->value())
        return std::unexpected{"Selected first-touch page mode is unavailable on the target."};
    const auto node = topology::unique_local_memory_node(context.topology, context.stable_ids, worker.id, *page);
    if (!node)
        return std::unexpected{"The first worker needs an unambiguous local node supporting the requested page size."};
    return *node;
}

auto monitoring_periodic(const Json& source) -> bool {
    const auto* enabled = lookup(source, "/execution/monitoring/sampling/during_observation/enabled");
    return enabled && *enabled == true;
}

// Pause validation at the authored placement boundary. The ordinary resolver checks
// complete Scenario composition, inputs, execution policy and layout on save/preview.
auto controller_viable(const Json& candidate, const ResourceResolutionContext& context)
    -> std::expected<void, std::string> {
    const auto execution = candidate.find("execution");
    if (execution == candidate.end() || !execution->is_object()) {
        return std::unexpected{"Add Scenario execution settings before placing workers or service CPUs."};
    }
    std::set<Cpu> occupied;
    scenario::collect_package_occupied_cpus(candidate, occupied);
    if (auto chosen = scenario::choose_package_controller(*execution, occupied, context); !chosen) {
        return std::unexpected{chosen.error().message};
    }
    return {};
}

auto role_cpus(const Json& role) -> Json {
    const auto entry = role.find("cpus");
    return entry != role.end() && entry->is_array() ? *entry : Json::array();
}

auto worker_nodes(const Json& role, const Json& cpus) -> Json {
    if (const auto nodes = role.find("numa_nodes"); nodes != role.end() && nodes->is_array() &&
        nodes->size() == cpus.size()) return *nodes;
    const auto node = role.find("numa_node");
    return std::vector<Json>(cpus.size(), node != role.end() ? *node : Json(nullptr));
}

void write_workers(Json& role, Json cpus) {
    if (role.contains("numa_nodes")) {
        const auto old_cpus = role_cpus(role);
        const auto old_nodes = worker_nodes(role, old_cpus);
        Json nodes = Json::array();
        for (const auto& cpu : cpus) {
            const auto old = std::ranges::find(old_cpus, cpu);
            nodes.push_back(old == old_cpus.end() ? Json(nullptr) :
                            old_nodes.at(static_cast<std::size_t>(old - old_cpus.begin())));
        }
        role["numa_nodes"] = std::move(nodes);
    }
    role["cpus"] = std::move(cpus);
}

auto roles_for(const Json& source, std::string_view role_pointer) -> const Json* {
    if (const auto last = role_pointer.rfind('/'); last != std::string_view::npos) {
        const auto* roles = lookup(source, role_pointer.substr(0, last));
        if (roles && roles->is_array()) return roles;
    }
    return nullptr;
}

struct WorkerRules {
    const ResourceResolutionContext& context;
    const TaskManifest* task;
    std::size_t maximum;
    std::set<Cpu> concurrent_cpus;
    std::optional<core::CoreId> mapping_service, monitor_service, controller;
    Json first_touch;

    auto check_cpu(Cpu id, bool first) const -> std::expected<const Unit*, std::string> {
        const auto* unit = find_cpu(context, id);
        if (!unit || !is_online(context.topology.machine(), id))
            return std::unexpected{"A selected worker CPU is offline or outside the effective CPU set."};
        if (concurrent_cpus.contains(id)) {
            return std::unexpected{"CPU " + std::to_string(id.value()) + " already runs another concurrent role."};
        }
        if (unit->core_id == mapping_service || unit->core_id == monitor_service)
            return std::unexpected{"A selected worker shares the physical core of a dedicated service CPU."};
        if (unit->core_id == controller)
            return std::unexpected{"A selected worker would use the dedicated controller core."};
        if (first && task) {
            if (task->mapping == MappingResource::first_touch) {
                const auto local = first_touch_memory(context, *unit, first_touch);
                if (!local) return std::unexpected{local.error()};
            } else if (task->shared_cpu_local && !local_memory(context, *unit)) {
                return std::unexpected{"The first worker needs an unambiguous local base-page memory node."};
            }
        }
        return unit;
    }

    auto check(const Json& cpus) const -> std::expected<void, std::string> {
        if (cpus.size() > maximum) {
            return std::unexpected{"This role allows at most " + std::to_string(maximum) +
                                   " worker CPUs. Choose a smaller group or replace workers."};
        }
        std::set<Cpu> ranks;
        std::set<core::CoreId> cores;
        for (const auto& cpu : cpus) {
            const auto id = linux_cpu(cpu);
            if (!id || !ranks.insert(*id).second)
                return std::unexpected{"Select a valid Linux CPU number for every worker."};
            const auto unit = check_cpu(*id, &cpu == &cpus.front());
            if (!unit) return std::unexpected{unit.error()};
            if (task && task->ranks.distinct_cores && !cores.insert((*unit)->core_id).second)
                return std::unexpected{"The task requires distinct physical cores for worker ranks."};
        }
        return {};
    }
};

auto worker_rules(const Json& source, const Json& role, std::string_view role_pointer,
                  const ResourceResolutionContext& context) -> WorkerRules {
    const auto* task = task_for(source, role, context);
    const auto mode = source.value("mode", "");
    const bool streaming = scenario::has_stream_chunks(source);
    const bool concurrent = mode == "mixed" || streaming;
    const auto maximum = streaming ? 1U : std::min<std::size_t>(1024U, task ? task->ranks.maximum : 1024U);
    std::set<Cpu> other_cpus;
    // A draft can grow one role at a time. The resolver checks equal ranks in completed compositions.
    if (const auto* roles = roles_for(source, role_pointer); concurrent && roles) {
        for (const auto& other : *roles) {
            if (&other == &role) continue;
            for (const auto& cpu : role_cpus(other)) scenario::collect_package_occupied_cpu(cpu, other_cpus);
        }
    }
    return {context, task, maximum, std::move(other_cpus),
            cpu_core(context, value_or(role, "/service_cpu", nullptr)),
            cpu_core(context, value_or(source, "/execution/monitoring/service_cpu", nullptr)),
            controller_core(source, context),
            task && task->mapping == MappingResource::first_touch ? first_touch_mode(source, role, *task) : Json(nullptr)};
}

auto selected_workers(const Json& role, std::span<const TopologyGroup> groups, PlacementRequest request,
                      const WorkerRules& rules) -> std::expected<Json, std::string> {
    Json cpus = request.replace_workers ? Json::array() : role_cpus(role);
    std::set<core::CoreId> taken;
    for (const auto& cpu : cpus) {
        if (const auto core = cpu_core(rules.context, cpu)) taken.insert(*core);
    }
    for (const auto& group : groups) {
        const auto before = cpus.size();
        std::string reason;
        for (const auto cpu : group.cpus) {
            if (std::ranges::contains(cpus, Json(cpu.value()))) continue;
            const auto unit = rules.check_cpu(cpu, cpus.empty());
            if (!unit) {
                if (reason.empty()) reason = unit.error();
                continue;
            }
            if (!request.all_siblings && group.kind != Group::cpu && !taken.insert((*unit)->core_id).second)
                continue;
            cpus.push_back(cpu.value());
        }
        if (cpus.size() == before)
            return std::unexpected{reason.empty() ? "This group contributes no additional online effective worker CPU."
                                                  : std::move(reason)};
    }
    return cpus;
}

auto local_first_touch(const Json& role, const ResourceResolutionContext& context, const Json& mode)
    -> std::expected<topology::LocalMemoryNode, std::string> {
    const auto cpus = role_cpus(role);
    if (cpus.size() != 1U) return std::unexpected{"Place the first-touch worker CPU before choosing memory."};
    const auto id = linux_cpu(cpus.front());
    const auto* unit = id ? find_cpu(context, *id) : nullptr;
    if (!unit || !is_online(context.topology.machine(), *id))
        return std::unexpected{"Place an online effective first-touch worker CPU before choosing memory."};
    return first_touch_memory(context, *unit, mode);
}

auto assign_memory(Json& role, const TopologyGroup& group, PlacementRequest request,
                   const ResourceResolutionContext& context, MappingResource mapping, const Json& first_touch,
                   bool streaming)
    -> std::expected<void, std::string> {
    if (group.kind != Group::numa || !group.linux_node)
        return std::unexpected{"Place memory on a Linux NUMA node."};
    const auto node = *group.linux_node;
    if (!usable_node(context, node))
        return std::unexpected{"This NUMA node needs online, effective, installed memory."};
    if (mapping == MappingResource::allocation)
        return std::unexpected{"Allocation probes do not have placed NUMA memory."};
    if (mapping == MappingResource::first_touch) {
        if (request.target != Target::role_memory)
            return std::unexpected{"First touch uses the worker's local bind placement."};
        const auto local = local_first_touch(role, context, first_touch);
        if (!local) return std::unexpected{local.error()};
        if (local->linux_id.value() != node)
            return std::unexpected{"Select the first-touch worker's exact local node with the requested page size."};
    }

    if (request.target == Target::role_memory) {
        role.erase("numa_nodes");
        role["numa_node"] = node;
    } else if (request.target == Target::rank_memory) {
        const auto cpus = role_cpus(role);
        if (request.rank >= cpus.size()) return std::unexpected{"Select a worker before assigning its memory."};
        auto nodes = worker_nodes(role, cpus);
        nodes.at(request.rank) = node;
        role.erase("numa_node");
        role["numa_nodes"] = std::move(nodes);
    } else {
        if (mapping != MappingResource::none)
            return std::unexpected{"Fresh mappings use their worker's bind placement."};
        const auto name = value_or(role, "/input_placement/policy", "bind");
        const auto policy = input_policy(name);
        if (!accepts_input_nodes(policy, streaming)) {
            if (streaming)
                return std::unexpected{"Streaming input memory uses bind. Choose bind in Input memory policy."};
            return std::unexpected{"Replica policies use the worker memory nodes."};
        }
        auto nodes = value_or(role, "/input_placement/nodes", Json::array());
        if (!nodes.is_array()) nodes = Json::array();
        if (std::ranges::contains(nodes, Json(node)))
            return std::unexpected{"This input memory node is already selected."};
        if (streaming && !stream_input_placement_supported(*policy, nodes.size() + 1U))
            return std::unexpected{"Streaming supports one explicit input memory node. Open Input memory and "
                                   "remove the current node before choosing another."};
        if (nodes.size() >= 1024U)
            return std::unexpected{"At most 1024 distinct input memory nodes are allowed."};
        nodes.push_back(node);
        role["input_placement"] = {{"policy", name}, {"nodes", std::move(nodes)}};
    }
    return {};
}

auto is_memory(Target target) -> bool {
    return target == Target::role_memory || target == Target::rank_memory ||
           target == Target::input_memory;
}

auto cpu_set_for(const topology::TopologySnapshot& snapshot, const topology::ProcessingUnitSet& members)
    -> core::CpuSet {
    core::CpuSet cpus;
    for (const auto& unit : snapshot.processing_units()) {
        if (members.contains(unit.id)) cpus = cpus.set_union(core::CpuSet{unit.linux_cpu_id});
    }
    return cpus;
}

} // namespace

auto topology_groups(const ResourceResolutionContext& context) -> std::vector<TopologyGroup> {
    const auto& snapshot = context.topology;
    std::vector<TopologyGroup> groups;
    for (const auto& node : snapshot.numa_nodes()) {
        const auto id = node.linux_node_id.value();
        auto label = id ? "NUMA node " + std::to_string(*id) : "NUMA node with no Linux ID";
        groups.push_back({Group::numa, std::move(label), cpu_set_for(snapshot, node.member_pus), id});
    }
    const auto cpu_order = [](const TopologyGroup& group) {
        const auto first = group.cpus.empty() ? std::numeric_limits<std::uint32_t>::max()
                                             : (*group.cpus.begin()).value();
        return std::pair{first, std::string_view{group.label}};
    };
    std::ranges::sort(groups, {}, &TopologyGroup::linux_node);
    auto first = groups.size();
    for (const auto& cache : snapshot.caches()) {
        if (cache.kind != topology::CacheKind::data && cache.kind != topology::CacheKind::unified)
            continue;
        auto cpus = cpu_set_for(snapshot, cache.member_pus);
        auto label = "L" + std::to_string(cache.level) +
            (cache.kind == topology::CacheKind::unified ? " unified cache" : " data cache");
        if (!cpus.empty()) label += " | CPUs " + cpus.to_range_list();
        groups.push_back({Group::cache, std::move(label), std::move(cpus), {}, cache.level});
    }
    std::ranges::sort(groups.begin() + static_cast<std::ptrdiff_t>(first), groups.end(), {}, cpu_order);
    first = groups.size();
    for (const auto& core : snapshot.cores()) {
        auto cpus = cpu_set_for(snapshot, snapshot.core_members(core.id));
        if (cpus.empty()) continue;
        const auto stable = context.stable_ids.core_id(core.id);
        auto label = stable ? "Core " + std::string{stable->get().value()} :
                              "Core " + cpus.to_range_list();
        groups.push_back({Group::core, std::move(label), std::move(cpus), {}});
    }
    std::ranges::sort(groups.begin() + static_cast<std::ptrdiff_t>(first), groups.end(), {}, cpu_order);
    first = groups.size();
    for (const auto& unit : snapshot.processing_units()) {
        groups.push_back({Group::cpu, "CPU " + std::to_string(unit.linux_cpu_id.value()),
                          core::CpuSet{unit.linux_cpu_id}, {}});
    }
    std::ranges::sort(groups.begin() + static_cast<std::ptrdiff_t>(first), groups.end(), {}, cpu_order);
    return groups;
}

namespace {

auto apply_worker_order(const Json& source, std::string_view role_pointer, const Json& cpus,
                        const WorkerRules& rules) -> std::expected<Json, std::string> {
    if (!cpus.is_array()) return std::unexpected{"Select an ordered list of worker CPUs."};
    if (const auto valid = rules.check(cpus); !valid) return std::unexpected{valid.error()};
    auto candidate = source;
    auto& placed = candidate.at(Json::json_pointer{std::string{role_pointer}});
    write_workers(placed, cpus);
    if (rules.task && rules.task->mapping == MappingResource::first_touch) {
        const auto node = value_or(placed, "/numa_node", nullptr);
        const auto local = local_first_touch(placed, rules.context, rules.first_touch);
        if (!local || node != local->linux_id.value()) placed.erase("numa_node");
    }
    if (!cpus.empty()) {
        if (const auto valid = controller_viable(candidate, rules.context); !valid)
            return std::unexpected{valid.error()};
    }
    return candidate;
}

auto preview_workers(const Json& source, const ResourceResolutionContext& context,
                     std::string_view role_pointer, std::span<const TopologyGroup> groups,
                     PlacementRequest request) -> std::expected<Json, std::string> {
    const auto* role = role_pointer.empty() ? nullptr : lookup(source, role_pointer);
    if (!role || !role->is_object()) return std::unexpected{"Select a Scenario role first."};
    const auto rules = worker_rules(source, *role, role_pointer, context);
    const auto cpus = selected_workers(*role, groups, request, rules);
    if (!cpus) return std::unexpected{cpus.error()};
    return apply_worker_order(source, role_pointer, *cpus, rules);
}

} // namespace

auto preview_worker_order(const Json& source, const ResourceResolutionContext& context,
                          std::string_view role_pointer, const Json& ordered_cpus)
    -> std::expected<Json, std::string> {
    const auto* role = role_pointer.empty() ? nullptr : lookup(source, role_pointer);
    if (!role || !role->is_object()) return std::unexpected{"Select a Scenario role first."};
    return apply_worker_order(source, role_pointer, ordered_cpus, worker_rules(source, *role, role_pointer, context));
}

auto preview_worker_team(const Json& source, const ResourceResolutionContext& context,
                         std::string_view role_pointer, std::span<const TopologyGroup> ordered_cores)
    -> std::expected<Json, std::string> {
    if (ordered_cores.empty() || std::ranges::any_of(ordered_cores, [](const auto& group) {
            return group.kind != Group::core;
        }))
        return std::unexpected{"Select physical cores for the worker team."};
    return preview_workers(source, context, role_pointer, ordered_cores,
                           {.target = Target::workers, .replace_workers = true});
}

auto preview_placement(const Json& source, const ResourceResolutionContext& context,
                       std::string_view role_pointer, const TopologyGroup& group,
                       PlacementRequest request) -> std::expected<Json, std::string> {
    const auto target = request.target;
    if (target == Target::workers) {
        if (request.all_siblings && group.kind == Group::cpu)
            return std::unexpected{"Place all SMT siblings on a core, cache or NUMA group."};
        return preview_workers(source, context, role_pointer, std::span{&group, 1U}, request);
    }
    auto candidate = source;
    const auto* role = role_pointer.empty() ? nullptr : lookup(source, role_pointer);
    const Json::json_pointer role_path{std::string{role_pointer}};
    if (target == Target::mapping_service || is_memory(target)) {
        if (!role || !role->is_object()) return std::unexpected{"Select a Scenario role first."};
    }
    if (is_memory(target)) {
        const auto* task = task_for(source, *role, context);
        const auto mapping = task ? task->mapping : MappingResource::none;
        const auto mode = mapping == MappingResource::first_touch ? first_touch_mode(source, *role, *task)
                                                                  : Json(nullptr);
        const auto valid = assign_memory(candidate.at(role_path), group, request, context, mapping, mode,
                                         scenario::has_stream_chunks(source));
        if (!valid) return std::unexpected{valid.error()};
        return candidate;
    } else if (target == Target::controller) {
        if ((group.kind != Group::core && group.kind != Group::cpu) || group.cpus.empty())
            return std::unexpected{"Place the controller on one physical core, or select its CPU."};
        const auto* unit = find_cpu(context, *group.cpus.begin());
        if (!unit || (group.kind == Group::cpu && !is_online(context.topology.machine(), unit->linux_cpu_id)))
            return std::unexpected{"Select an online effective CPU or a core that contains one."};
        const auto stable = context.stable_ids.core_id(unit->core_id);
        if (!stable) return std::unexpected{"The physical core has no stable topology ID."};
        candidate["execution"]["controller_core"] = stable->get().value();
    } else {
        if (group.kind != Group::cpu || group.cpus.empty())
            return std::unexpected{"Place a dedicated service on an individual Linux CPU."};
        const auto id = *group.cpus.begin();
        const auto* unit = find_cpu(context, id);
        if (!unit || !is_online(context.topology.machine(), id))
            return std::unexpected{"The service CPU must be online and effective."};
        // Test the new core against the other occupied cores, excluding the old service assignment.
        if (target == Target::mapping_service) {
            if (mapping_kind(source, *role, context) == MappingResource::none)
                return std::unexpected{"This task does not use a dedicated mapping service CPU."};
            candidate.at(role_path).erase("service_cpu");
        } else {
            if (!monitoring_periodic(source))
                return std::unexpected{"Enable during-observation monitoring before placing its service."};
            candidate["execution"]["monitoring"].erase("service_cpu");
        }
        std::set<Cpu> occupied;
        scenario::collect_package_occupied_cpus(candidate, occupied);
        if (std::ranges::any_of(context.topology.processing_units(), [&](const auto& worker) {
                return worker.core_id == unit->core_id && occupied.contains(worker.linux_cpu_id);
            }))
            return std::unexpected{"A worker or another service already uses this physical core."};
        if (target == Target::mapping_service) candidate.at(role_path)["service_cpu"] = id.value();
        else candidate["execution"]["monitoring"]["service_cpu"] = id.value();
    }
    if (auto valid = controller_viable(candidate, context); !valid)
        return std::unexpected{valid.error()};
    return candidate;
}

namespace {

auto target_name(Target target) -> std::string_view {
    switch (target) {
        case Target::workers: return "Workers";
        case Target::role_memory: return "Role memory";
        case Target::rank_memory: return "Rank memory";
        case Target::input_memory: return "Input memory";
        case Target::mapping_service: return "Mapping service";
        case Target::monitoring_service: return "Monitoring service";
        case Target::controller: return "Controller";
    }
    return {};
}

auto group_name(Group kind) -> std::string_view {
    switch (kind) {
        case Group::numa: return "NUMA nodes";
        case Group::cache: return "Cache groups";
        case Group::core: return "Physical cores";
        case Group::cpu: return "Linux CPUs";
    }
    return {};
}

auto setting_label(const Json& value, std::string_view missing) -> std::string {
    if (value.is_null() || (value.is_array() && value.empty())) return std::string{missing};
    if (value.is_string()) return value.get<std::string>();
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

struct Assignment {
    PlacementRequest request;
    std::string detail;
    std::string payload;
    bool worker_team{};
};

struct TopologyMapRow {
    std::size_t group;
    std::size_t line;
    unsigned indent;
};

auto map_rows(const std::vector<TopologyGroup>& groups) -> std::vector<TopologyMapRow> {
    const auto root = groups.size();
    std::vector<std::size_t> nodes(groups.size(), root);
    for (std::size_t i = 0; i < root; ++i) {
        if (groups[i].kind == Group::numa || groups[i].cpus.empty()) continue;
        for (std::size_t n = 0; n < root && groups[n].kind == Group::numa; ++n) {
            if (!groups[i].cpus.is_subset_of(groups[n].cpus)) continue;
            if (nodes[i] != root) {
                nodes[i] = root; // Report overlapping NUMA membership in the item detail.
                break;
            }
            nodes[i] = n;
        }
    }
    std::vector<std::vector<std::size_t>> children(root + 1U);
    for (std::size_t i = 0; i < root; ++i) {
        const auto& group = groups[i];
        auto parent = nodes[i];
        if (!group.cpus.empty() && group.kind != Group::numa) {
            for (std::size_t j = 0; j < root; ++j) {
                const auto& other = groups[j];
                if (i == j || nodes[i] != nodes[j]) continue;
                if (group.kind == Group::cpu && other.kind == Group::core &&
                    group.cpus.is_subset_of(other.cpus)) {
                    parent = j;
                    break;
                }
                if (other.kind != Group::cache || other.cache_level <= group.cache_level ||
                    !group.cpus.is_subset_of(other.cpus))
                    continue;
                if (parent == nodes[i] || std::pair{other.cpus.cardinality(), other.cache_level} <
                    std::pair{groups[parent].cpus.cardinality(), groups[parent].cache_level}) {
                    parent = j;
                }
            }
        }
        children[parent].push_back(i);
    }
    std::vector<TopologyMapRow> rows;
    const auto append = [&](const auto& self, std::size_t i, unsigned depth, std::size_t line) -> void {
        rows.push_back({i, line, depth});
        for (const auto child : children[i]) {
            const bool sibling = groups[i].kind == Group::core && groups[child].kind == Group::cpu;
            self(self, child, sibling ? depth : depth + 1U, sibling ? line : child);
        }
    };
    for (const auto i : children[root]) append(append, i, 0U, i);
    return rows;
}

struct PlacementPage : std::enable_shared_from_this<PlacementPage> {
    Session& session;
    std::string role_path;
    Action done;
    std::vector<TopologyGroup> groups;
    std::vector<TopologyMapRow> map;
    std::optional<Group> layer;
    std::optional<TopologyGroup> scope;
    std::vector<std::pair<std::optional<Group>, std::optional<TopologyGroup>>> parents;
    std::vector<TopologyGroup> team;
    std::optional<std::size_t> rank;

    PlacementPage(Session& session_, std::string role_path_, Action done_)
        : session{session_}, role_path{std::move(role_path_)}, done{std::move(done_)},
          groups{topology_groups(session.context())}, map{map_rows(groups)} {
        if (role_path.empty()) layer = Group::core;
    }

    auto role() const -> const Json* {
        return role_path.empty() ? nullptr : lookup(session.draft, role_path);
    }

    auto role_name() const -> std::string {
        if (const auto* active = role(); active && active->is_object()) {
            return setting_label(value_or(*active, "/id", nullptr), role_path);
        }
        return {};
    }

    auto back() -> Action {
        return [self = shared_from_this()] { self->render(); };
    }

    void display(std::string message) {
        render();
        session.message = std::move(message);
    }

    void commit(Json prospect, std::string message) {
        session.draft = std::move(prospect);
        render();
        session.message = std::move(message);
        session.persist();
    }

    auto preview(const TopologyGroup& group, PlacementRequest request) const
        -> std::expected<Json, std::string> {
        return preview_placement(session.draft, session.context(), role_path, group, request);
    }

    auto selected_controller() const -> std::expected<scenario::PackageControllerPlacement, core::Error> {
        std::set<Cpu> occupied;
        scenario::collect_package_occupied_cpus(session.draft, occupied);
        return scenario::choose_package_controller(
            value_or(session.draft, "/execution", Json::object()), occupied, session.context());
    }

    auto assignments(const std::expected<scenario::PackageControllerPlacement, core::Error>& selected) const
        -> std::vector<Assignment> {
        std::vector<Assignment> result;
        const auto add = [&](PlacementRequest request, std::string detail) {
            result.push_back({request, std::move(detail), std::string{target_name(request.target)}});
        };
        if (const auto* active = role(); active && active->is_object()) {
            const auto cpus = role_cpus(*active);
            add({Target::workers}, "One eligible CPU per core | Ordered Linux CPUs " +
                                   setting_label(cpus, "unassigned"));
            result.push_back({{Target::workers, true}, "Use every eligible SMT sibling in the target group",
                              "All SMT siblings"});
            result.push_back({{Target::workers}, team.empty() ? "Select the full team in physical core rank order" :
                                  std::to_string(team.size()) + " physical cores selected", "Worker team", true});
            const auto mapping = mapping_kind(session.draft, *active, session.context());
            if (mapping != MappingResource::allocation) {
                add({Target::role_memory}, "Node " +
                    setting_label(value_or(*active, "/numa_node", nullptr),
                                  active->contains("numa_nodes") ? "set per rank" : "unassigned"));
            }
            if (mapping == MappingResource::none) {
                if (rank && *rank < cpus.size()) {
                    add({Target::rank_memory, false, false, *rank},
                        "CPU " + setting_label(cpus.at(*rank), "unassigned") + " -> node " +
                        setting_label(worker_nodes(*active, cpus).at(*rank), "unassigned"));
                }
                const auto policy = value_or(*active, "/input_placement/policy", "bind");
                if (accepts_input_nodes(input_policy(policy), scenario::has_stream_chunks(session.draft))) {
                    const std::string_view inherited = policy == "bind" ? "from role or rank memory" : "unassigned";
                    add({Target::input_memory}, "Nodes " +
                        setting_label(value_or(*active, "/input_placement/nodes", nullptr), inherited));
                }
            } else {
                add({Target::mapping_service}, "Linux CPU " +
                    setting_label(value_or(*active, "/service_cpu", nullptr), "unassigned"));
            }
        }
        if (monitoring_periodic(session.draft)) {
            add({Target::monitoring_service}, "Linux CPU " +
                setting_label(value_or(session.draft, "/execution/monitoring/service_cpu", nullptr), "automatic"));
        }
        const auto setting = value_or(session.draft, "/execution/controller_core", nullptr);
        auto detail = setting_label(setting, "Automatic");
        if (selected) detail += " | CPU " + std::to_string(selected->cpu.value());
        else detail += " | " + selected.error().message;
        add({Target::controller}, std::move(detail));
        return result;
    }

    auto team_preview(std::span<const TopologyGroup> cores) const -> std::expected<Json, std::string> {
        return preview_worker_team(session.draft, session.context(), role_path, cores);
    }

    auto edited_team(const TopologyGroup& group) const -> std::expected<std::vector<TopologyGroup>, std::string> {
        if (group.kind != Group::core)
            return std::unexpected{"Select a physical core for the worker team."};
        auto next = team;
        const auto existing = std::ranges::find(next, group.cpus, &TopologyGroup::cpus);
        if (existing != next.end()) {
            next.erase(existing);
        } else {
            next.push_back(group);
            if (const auto candidate = team_preview(next); !candidate)
                return std::unexpected{candidate.error()};
        }
        return next;
    }

    auto rejection(const TopologyGroup& group, const Assignment& assignment) const -> std::string {
        if (assignment.worker_team) {
            const auto candidate = edited_team(group);
            return candidate ? std::string{} : candidate.error();
        }
        const auto candidate = preview(group, assignment.request);
        return candidate ? std::string{} : candidate.error();
    }

    void worker_preview(Json prospect, std::string label) {
        const auto cpus = role_cpus(prospect.at(Json::json_pointer{role_path}));
        const auto nodes = worker_nodes(prospect.at(Json::json_pointer{role_path}), cpus);
        session.page("Worker assignment preview", back());
        const auto self = shared_from_this();
        session.action("Apply workers to " + role_name(),
            [self, prospect = std::move(prospect)]() mutable {
                self->rank.reset();
                self->team.clear();
                self->commit(std::move(prospect), "Worker placement updated.");
            }, std::move(label));
        for (std::size_t i = 0; i < cpus.size(); ++i) {
            session.note("Rank " + std::to_string(i) + " -> CPU " + cpus.at(i).dump(),
                         "Memory node " + setting_label(nodes.at(i), "unassigned"));
        }
    }

    void perform(const TopologyGroup& group, const Assignment& assignment) {
        if (assignment.worker_team) {
            auto next = edited_team(group);
            if (!next) {
                display(next.error());
            } else {
                const bool added = next->size() > team.size();
                team = std::move(*next);
                display(group.label + (added ? " added to worker team." : " removed from worker team."));
            }
            return;
        }
        auto candidate = preview(group, assignment.request);
        if (!candidate) display(candidate.error());
        else if (assignment.request.target == Target::workers)
            worker_preview(std::move(*candidate), group.label);
        else commit(std::move(*candidate), assignment.payload + " placement updated.");
    }

    void open_layer(std::optional<Group> kind, std::optional<TopologyGroup> region = {}) {
        if (region) parents.emplace_back(layer, scope);
        else parents.clear();
        layer = kind;
        scope = std::move(region);
        render();
    }

    void parent_layer() {
        auto [kind, region] = std::move(parents.back());
        parents.pop_back();
        layer = kind;
        scope = std::move(region);
        render();
    }

    void browse() {
        const std::array kinds{Group::numa, Group::cache, Group::core, Group::cpu};
        std::vector<std::string> labels{"Machine overview"};
        for (const auto kind : kinds) labels.emplace_back(group_name(kind));
        session.choose("Browse topology", std::move(labels), [self = shared_from_this(), kinds](std::size_t i) {
            self->open_layer(i ? std::optional{kinds.at(i - 1U)} : std::nullopt);
        }, back());
    }

    void group_menu(const TopologyGroup& group) {
        session.page(group.label, back());
        const auto self = shared_from_this();
        session.note("Membership and assignments", describe_group(group));
        if (group.kind == Group::numa && !group.cpus.empty()) {
            session.action("Show cache groups in this node", [self, group] { self->open_layer(Group::cache, group); });
        }
        if ((group.kind == Group::numa || group.kind == Group::cache) && !group.cpus.empty()) {
            session.action("Show physical cores", [self, group] { self->open_layer(Group::core, group); });
        }
        if (group.kind != Group::cpu && !group.cpus.empty()) {
            session.action(group.kind == Group::core ? "Show individual SMT siblings" : "Show Linux CPUs",
                           [self, group] { self->open_layer(Group::cpu, group); });
        }
        for (const auto& assignment : assignments(selected_controller())) {
            if (assignment.worker_team && group.kind != Group::core) continue;
            if (assignment.request.all_siblings && group.kind == Group::cpu) continue;
            const auto reason = rejection(group, assignment);
            const auto selected = std::ranges::contains(team, group.cpus, &TopologyGroup::cpus);
            const auto label = assignment.worker_team ? (selected ? "Remove core from worker team" :
                                                                    "Add core to worker team")
                                                      : "Place " + assignment.payload + " here";
            session.action(label, [self, group, assignment] { self->perform(group, assignment); },
                           reason.empty() ? assignment.detail : reason);
            if (const auto* active = role(); assignment.request.target == Target::workers &&
                !assignment.worker_team && active && !role_cpus(*active).empty()) {
                auto replacement = assignment;
                replacement.request.replace_workers = true;
                const auto blocked = rejection(group, replacement);
                session.action(assignment.request.all_siblings ? "Replace workers with all eligible SMT siblings" :
                                                                "Replace workers with this selection",
                               [self, group, replacement] { self->perform(group, replacement); }, blocked);
            }
        }
    }

    void manage_rank(std::size_t index) {
        const auto cpus = role_cpus(*role());
        session.page("Rank " + std::to_string(index) + " placement", back());
        const auto self = shared_from_this();
        const auto reorder = [self, cpus](std::size_t first, std::size_t second) {
            auto next = cpus;
            std::swap(next.at(first), next.at(second));
            self->update_worker_order(next, "Worker rank order updated.");
        };
        if (index) session.action("Move rank earlier", [reorder, index] { reorder(index, index - 1U); });
        if (index + 1U < cpus.size()) {
            session.action("Move rank later", [reorder, index] { reorder(index, index + 1U); });
        }
        session.action("Remove worker CPU", [self, index, cpus] {
            auto next = cpus;
            next.erase(index);
            self->update_worker_order(next, "Worker CPU removed.");
        });
    }

    void update_worker_order(const Json& cpus, std::string message) {
        auto candidate = preview_worker_order(session.draft, session.context(), role_path, cpus);
        if (!candidate) {
            display(candidate.error());
            return;
        }
        rank.reset();
        team.clear();
        commit(std::move(*candidate), std::move(message));
    }

    void ranks(bool memory) {
        const auto cpus = role_cpus(*role());
        if (cpus.empty()) {
            display("Place worker CPUs before choosing a rank.");
            return;
        }
        std::vector<std::string> labels;
        const auto nodes = worker_nodes(*role(), cpus);
        for (std::size_t i = 0; i < cpus.size(); ++i) {
            labels.push_back("Rank " + std::to_string(i) + " | CPU " + cpus.at(i).dump() +
                             " | memory node " + setting_label(nodes.at(i), "unassigned"));
        }
        session.choose(memory ? "Choose a rank to place its memory" : "Choose a worker rank", std::move(labels),
            [self = shared_from_this(), memory](std::size_t index) {
                if (memory) {
                    self->rank = index;
                    self->open_layer(Group::numa);
                } else {
                    self->manage_rank(index);
                }
            }, back());
    }

    void set_policy(InputPolicy policy) {
        if (scenario::has_stream_chunks(session.draft) && !stream_input_placement_supported(policy, 0U)) {
            input_policy_menu();
            session.message = "Streaming input memory uses bind. Choose bind in Input memory policy.";
            return;
        }
        const std::string name{kernel::packages::input_policy_name(policy)};
        const auto self = shared_from_this();
        if (policy == InputPolicy::replica_per_worker || policy == InputPolicy::replica_per_node) {
            const auto current = value_or(*role(), "/input_placement/replica_group", nullptr);
            session.input("Replica group name", setting_label(current, ""), [self, name](std::string group) {
                if (!scenario::ScenarioIdentifier::parse(group)) {
                    self->display("Enter a valid Scenario identifier for the shared replica group.");
                    return;
                }
                auto prospect = self->session.draft;
                prospect.at(Json::json_pointer{self->role_path})["input_placement"] =
                    {{"policy", name}, {"replica_group", std::move(group)}};
                self->commit(std::move(prospect), "Input replica policy updated.");
            }, back(), {{"view", "placement-replica"}, {"role", role_path}, {"policy", name}});
            return;
        }
        auto prospect = session.draft;
        auto& active = prospect.at(Json::json_pointer{role_path});
        if (policy == InputPolicy::bind) active.erase("input_placement");
        else active["input_placement"] = {{"policy", name}, {"nodes", Json::array()}};
        commit(std::move(prospect), "Input policy updated. Place its memory on the map.");
    }

    void input_policy_menu() {
        const auto self = shared_from_this();
        const bool streaming = scenario::has_stream_chunks(session.draft);
        session.page("Input memory policy", back());
        const auto add = [&](InputPolicy policy, std::string label, std::string detail = {}) {
            if (!streaming || stream_input_placement_supported(policy, 0U))
                session.action(std::move(label), [self, policy] { self->set_policy(policy); }, std::move(detail));
        };
        add(InputPolicy::bind, "Bind to role or rank memory",
            streaming ? "To bind input separately, place Input memory on one NUMA node." : "");
        add(InputPolicy::interleave, "Interleave explicit NUMA nodes", "Place input memory on at least two distinct nodes.");
        add(InputPolicy::replica_per_worker, "Replica per worker");
        add(InputPolicy::replica_per_node, "Replica per NUMA node");
        const auto nodes = value_or(*role(), "/input_placement/nodes", Json::array());
        if (nodes.is_array()) {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                session.action("Remove input memory node " + nodes.at(i).dump(), [self, i] {
                    auto prospect = self->session.draft;
                    auto& role = prospect.at(Json::json_pointer{self->role_path});
                    auto& policy = role.at("input_placement");
                    auto& targets = policy.at("nodes");
                    targets.erase(i);
                    if (targets.empty() && policy.at("policy") == "bind") role.erase("input_placement");
                    self->commit(std::move(prospect), "Input memory node removed.");
                });
            }
        }
    }

    void controller_menu() {
        const auto self = shared_from_this();
        auto choices = scenario::package_controller_choices(session.draft, session.context());
        select_controller(session, choices, [self, choices](std::size_t selected) {
            auto prospect = self->session.draft;
            auto& execution = prospect["execution"];
            if (selected == 0U) execution.erase("controller_core");
            else execution["controller_core"] = choices.at(selected - 1U).core.value();
            if (auto checked = controller_viable(prospect, self->session.context()); !checked) {
                self->display(checked.error());
            } else {
                self->commit(std::move(prospect), "Controller selection updated.");
            }
        }, back());
    }

    void monitoring_menu() {
        const auto self = shared_from_this();
        session.page("Monitoring service placement", back());
        session.action("Use automatic service CPU", [self] {
            auto prospect = self->session.draft;
            prospect.at("execution").at("monitoring").erase("service_cpu");
            self->commit(std::move(prospect), "Monitoring service placement is automatic.");
        });
        session.action("Browse Linux CPUs", [self] { self->open_layer(Group::cpu); });
    }

    auto describe_group(const TopologyGroup& group, std::string* annotations = nullptr) const -> std::string {
        const auto context = session.context();
        const auto& machine = context.topology.machine();
        const auto& effective = machine.effective_cpus.value();
        const auto eligible = effective ? group.cpus.intersection(*effective).intersection(machine.online_cpus)
                                        : core::CpuSet{};
        auto detail = group.cpus.empty() ? "No Linux CPUs" : "Linux CPUs " + group.cpus.to_range_list();
        if (eligible != group.cpus)
            detail += eligible.empty() ? " | no online effective CPUs" :
                                        " | online effective " + eligible.to_range_list();
        if (group.kind == Group::cpu && !group.cpus.empty()) {
            if (const auto* unit = find_cpu(context, *group.cpus.begin())) {
                const auto core = context.stable_ids.core_id(unit->core_id);
                if (core) detail += " | " + std::string{core->get().value()};
                if (const auto local = local_memory(context, *unit))
                    detail += " | local memory node " + std::to_string(local->linux_id.value());
            }
        }
        const bool can_hold_memory = group.linux_node && usable_node(context, *group.linux_node);
        if (group.linux_node) detail += can_hold_memory ? " | eligible memory" : " | no usable memory";
        const auto annotate = [annotations](const std::string& label) {
            if (!annotations) return;
            if (!annotations->empty()) *annotations += ", ";
            *annotations += label;
        };
        if (const auto* roles = roles_for(session.draft, role_path)) {
            for (const auto& item : *roles) {
                core::CpuSet worker_cpus;
                for (const auto& item_cpu : role_cpus(item)) {
                    const auto cpu = linux_cpu(item_cpu);
                    if (cpu && group.cpus.contains(*cpu))
                        worker_cpus = worker_cpus.set_union(core::CpuSet{*cpu});
                }
                const auto id = setting_label(value_or(item, "/id", nullptr), "Role");
                if (!worker_cpus.empty()) {
                    detail += " | " + id + " CPUs " + worker_cpus.to_range_list();
                    if (group.kind == Group::cpu) annotate(id);
                }
                if (!group.linux_node) continue;
                const Json node = *group.linux_node;
                const auto nodes = value_or(item, "/numa_nodes", Json::array());
                const auto inputs = value_or(item, "/input_placement/nodes", Json::array());
                if (value_or(item, "/numa_node", nullptr) == node ||
                    (nodes.is_array() && std::ranges::contains(nodes, node))) {
                    detail += " | " + id + " role memory";
                    annotate(id + " memory");
                }
                if (inputs.is_array() && std::ranges::contains(inputs, node)) {
                    detail += " | " + id + " input memory";
                    annotate(id + " input");
                }
            }
        }
        return detail;
    }

    void render() {
        const auto self = shared_from_this();
        const auto name = role_name();
        Action go_back = parents.empty() ? done : Action{[self] { self->parent_layer(); }};
        session.page(name.empty() ? "Topology placement" : "Topology placement for " + name, std::move(go_back));
        session.action("Done editing placement", done, {}, "Assignments");
        const auto controller = selected_controller();
        const auto targets = std::make_shared<const std::vector<Assignment>>(assignments(controller));
        for (const auto& assignment : *targets) {
            const auto target = assignment.request.target;
            auto& row = session.action(assignment.payload, [self, target, assignment] {
                if (assignment.worker_team || assignment.request.all_siblings)
                    self->open_layer(std::nullopt);
                else if (target == Target::controller) self->controller_menu();
                else if (target == Target::workers) self->ranks(false);
                else if (target == Target::rank_memory) self->ranks(true);
                else if (target == Target::input_memory) self->input_policy_menu();
                else if (target == Target::monitoring_service) self->monitoring_menu();
                else self->open_layer(is_memory(target) ? Group::numa : Group::cpu);
            }, assignment.detail, "Assignments");
            row.payload = assignment.payload;
        }
        if (!team.empty()) {
            session.action("Preview worker team", [self] {
                auto candidate = self->team_preview(self->team);
                if (!candidate) self->display(candidate.error());
                else self->worker_preview(std::move(*candidate), "Replace workers in physical core selection order");
            }, {}, "Assignments");
            session.action("Clear worker team", [self] {
                self->team.clear();
                self->display("Worker team selection cleared.");
            }, {}, "Assignments");
        }
        if (const auto* active = role(); active && active->is_object() &&
            mapping_kind(session.draft, *active, session.context()) == MappingResource::none) {
            session.action("Choose rank for memory placement", [self] { self->ranks(true); }, {}, "Assignments");
            const auto policy = value_or(*active, "/input_placement/policy", "bind");
            if (!accepts_input_nodes(input_policy(policy), scenario::has_stream_chunks(session.draft))) {
                session.action("Input memory policy", [self] { self->input_policy_menu(); },
                               setting_label(policy, "bind"), "Assignments");
            }
        }
        const auto view = layer ? std::string{group_name(*layer)} : std::string{"Machine overview"};
        session.action("Browse topology", [self] { self->browse(); },
                       scope ? view + " within " + scope->label : view, "Topology map");
        for (const auto& item : map) {
            auto group = groups[item.group];
            if (layer && group.kind != *layer) continue;
            const bool controller_core = group.kind == Group::core && controller &&
                                         group.cpus.contains(controller->cpu);
            if (scope) {
                group.cpus = group.cpus.intersection(scope->cpus);
                if (group.cpus.empty()) continue;
            }
            auto label = group.label;
            if (group.kind == Group::core) {
                const auto selected = std::ranges::find(team, group.cpus, &TopologyGroup::cpus);
                if (selected != team.end())
                    label += " [team " + std::to_string(selected - team.begin() + 1) + "]";
            }
            if (controller_core) label += " [controller]";
            std::string annotations;
            auto detail = describe_group(group, &annotations);
            if (!annotations.empty()) label += " [" + annotations + "]";
            auto& row = session.action(group.label, [self, group] { self->group_menu(group); },
                                       std::move(detail), "Topology map");
            row.topology_tile = Row::TopologyTile{item.line, layer ? 0U : item.indent, std::move(label)};
            row.accepts = [self, group, targets, previous = std::string{}, valid = false](const std::string& payload) mutable {
                if (payload != previous) {
                    const auto it = std::ranges::find(*targets, payload, &Assignment::payload);
                    valid = it != targets->end() && self->rejection(group, *it).empty();
                    previous = payload;
                }
                return valid;
            };
            row.drop = [self, group, targets](std::string payload) {
                const auto it = std::ranges::find(*targets, payload, &Assignment::payload);
                if (it != targets->end()) self->perform(group, *it);
            };
            row.rejection = [self, group, targets](const std::string& payload) {
                const auto it = std::ranges::find(*targets, payload, &Assignment::payload);
                if (it != targets->end()) return self->rejection(group, *it);
                return std::string{"This topology group cannot receive that item."};
            };
        }
    }
};

} // namespace

void select_controller(Session& session, std::vector<scenario::PackageControllerPlacement> choices,
                       std::function<void(std::size_t)> selected, Action cancel) {
    session.page("Controller physical core", std::move(cancel));
    if (choices.empty()) {
        session.note("No separate core is available", "Move workers or a dedicated service to free a physical core.");
        return;
    }
    const auto pick = std::make_shared<std::function<void(std::size_t)>>(std::move(selected));
    session.action("Automatic", [pick] { (*pick)(0U); },
                   "Linux CPU " + std::to_string(choices.front().cpu.value()));
    for (std::size_t i = 0; i < choices.size(); ++i) {
        session.action(std::string{choices.at(i).core.value()}, [pick, i] { (*pick)(i + 1U); },
                       "Linux CPU " + std::to_string(choices.at(i).cpu.value()));
    }
}

void placement(Session& session, std::string role_pointer, Action done) {
    std::make_shared<PlacementPage>(session, std::move(role_pointer), std::move(done))->render();
}

void restore_placement_field(Session& session, const Json& address) {
    const auto path = address.find("role");
    const auto policy = address.find("policy");
    const auto restored_policy = policy == address.end() ? std::nullopt : input_policy(*policy);
    if (path == address.end() || !path->is_string() ||
        (restored_policy != InputPolicy::replica_per_worker && restored_policy != InputPolicy::replica_per_node)) {
        session.message = "The saved input memory field cannot be reopened.";
        return;
    }
    const auto& role_path = path->get_ref<const std::string&>();
    const auto has_role = [&](const std::string& prefix, const Json& roles) {
        if (!roles.is_array()) return false;
        for (std::size_t i = 0; i < roles.size(); ++i)
            if (roles[i].is_object() && role_path == prefix + std::to_string(i)) return true;
        return false;
    };
    const auto cases = value_or(session.draft, "/cases", Json::array());
    bool found = has_role("/roles/", value_or(session.draft, "/roles", Json::array()));
    for (std::size_t i = 0; !found && cases.is_array() && i < cases.size(); ++i) {
        found = has_role("/cases/" + std::to_string(i) + "/roles/",
                         value_or(cases[i], "/roles", Json::array()));
    }
    if (!found) {
        session.message = "The saved input memory role is unavailable.";
        return;
    }
    const auto page = std::make_shared<PlacementPage>(session, role_path, [&session] { edit_scenario(session); });
    if (mapping_kind(session.draft, *page->role(), session.context()) != MappingResource::none) {
        session.message = "The saved role does not support input replicas.";
        return;
    }
    page->set_policy(*restored_policy);
}

} // namespace epycbench::cli::tui
