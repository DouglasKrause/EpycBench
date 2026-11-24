#include "scenario/package_scenario.hpp"
#include "scenario/package_mixed.hpp"
#include "epycbench/scenario/identifier.hpp"
#include "epycbench/topology/worker_locality.hpp"
#include "measurement/package_monitoring.hpp"
#include "kernel/package_mapping.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace epycbench::scenario {
namespace {
using Json = nlohmann::json;
using Catalog = kernel::packages::Catalog;
using kernel::packages::MappingResource;

auto invalid(std::string path, std::string message) -> core::Error {
    return {core::ErrorCategory::scenario,
            core::StableErrorCode{"scenario.package.invalid"},
            std::move(message),
            "resolve_package",
            {},
            {{"path", std::move(path)}},
            core::ErrorSeverity::error,
            core::ContinuationPolicy::abort_command};
}

bool fields(const Json& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object())
        return false;
    return std::ranges::all_of(object.items(), [&](const auto& field) {
        return std::ranges::find(allowed, field.key()) != allowed.end();
    });
}

bool natural(const Json& value, std::uint64_t low, std::uint64_t high) {
    return value.is_number_unsigned() && value.get<std::uint64_t>() >= low &&
           value.get<std::uint64_t>() <= high;
}

bool fraction(const Json& value, bool open_interval) {
    if (!fields(value, {"numerator", "denominator"}) ||
        !natural(value.at("numerator"), open_interval ? 1U : 0U, UINT64_MAX) ||
        !natural(value.at("denominator"), 1U, UINT64_MAX))
        return false;
    const auto numerator = value.at("numerator").get<std::uint64_t>();
    const auto denominator = value.at("denominator").get<std::uint64_t>();
    if (!open_interval)
        return numerator <= denominator;
    return numerator < denominator &&
           static_cast<double>(numerator) / static_cast<double>(denominator) < 1.0;
}

bool normalize_monitoring(Json& settings) {
    namespace monitor = measurement::package_monitoring;
    if (!fields(settings, {"required", "optional", "sampling", "unavailable_optional_source", "service_cpu"}))
        return false;
    std::set<std::string> seen;
    for (const char* field : {"required", "optional"}) {
        if (!settings.contains(field))
            settings[field] = Json::array();
        auto& channels = settings.at(field);
        if (!channels.is_array() || channels.size() > monitor::channel_names.size())
            return false;
        for (const auto& entry : channels) {
            if (!entry.is_string())
                return false;
            const auto& name = entry.get_ref<const std::string&>();
            if (!std::ranges::contains(monitor::channel_names, name) || !seen.insert(name).second)
                return false;
        }
        std::sort(channels.begin(), channels.end());
    }
    if (!settings.contains("unavailable_optional_source"))
        settings["unavailable_optional_source"] = "fail";
    const auto& unavailable = settings.at("unavailable_optional_source");
    if (unavailable != "fail" && unavailable != "warn" && unavailable != "disable")
        return false;
    if (!settings.contains("sampling"))
        settings["sampling"] = Json::object();
    auto& sampling = settings.at("sampling");
    if (!fields(sampling, {"before_observation", "after_observation", "during_observation"}))
        return false;
    for (const char* field : {"before_observation", "after_observation"}) {
        if (!sampling.contains(field))
            sampling[field] = true;
        if (!sampling.at(field).is_boolean())
            return false;
    }
    if (!sampling.contains("during_observation"))
        sampling["during_observation"] = Json::object();
    auto& during = sampling.at("during_observation");
    if (!fields(during, {"enabled", "interval_ns"}))
        return false;
    if (!during.contains("enabled"))
        during["enabled"] = false;
    if (!during.at("enabled").is_boolean())
        return false;
    const bool periodic = during.at("enabled").get<bool>();
    return periodic == during.contains("interval_ns") &&
           (!periodic || natural(during.at("interval_ns"), 1000U, 3600000000000ULL)) &&
           (!settings.contains("service_cpu") ||
            (periodic && natural(settings.at("service_cpu"), 0, UINT32_MAX)));
}

auto digest(const Json& value) -> core::Sha256Digest {
    // Sorted JSON objects and exact integer tokens also cover the full uint64 domain.
    return core::sha256(value.dump());
}

std::string scope(std::string_view mode, bool streaming) {
    if (mode == "mixed")
        return "concurrent-group-wall-time";
    if (mode == "pipeline")
        return streaming ? "stream-release-to-completion" : "sequential-workflow-wall-time";
    return "role-enclosing-wall-time";
}

auto parameter_path(std::string_view root, const std::string& id, std::string_view field,
                    const std::string& name) {
    return (Json::json_pointer{} / std::string(root) / id / std::string(field) / name).to_string();
}

auto mapping_plan(MappingResource kind, const Json& role, const Json& parameters,
                  const ResourceResolutionContext& context, const Json& coordinate,
                  const std::string& workload, std::vector<std::string>* related_settings)
    -> std::expected<Json, core::Error> {
    constexpr std::string_view pointer = "/roles/0/service_cpu";
    if (!role.contains("service_cpu") || !natural(role.at("service_cpu"), 0, UINT32_MAX))
        return std::unexpected{invalid(std::string{pointer}, "Select a Linux service CPU")};
    const core::LinuxCpuId service_cpu{role.at("service_cpu").get<std::uint32_t>()};
    const auto units = context.topology.processing_units();
    const auto service = std::ranges::find(units, service_cpu, &topology::ProcessingUnit::linux_cpu_id);
    const auto& machine = context.topology.machine();
    const bool shares_worker_core = service != units.end() && coordinate.at("core") == service->core_id.value();
    if (service == units.end() || !machine.online_cpus.contains(service_cpu) ||
        !machine.effective_cpus.value()->contains(service_cpu) || shares_worker_core) {
        if (related_settings && shares_worker_core)
            related_settings->assign({std::string(pointer), "/roles/0/cpus"});
        return std::unexpected{invalid(std::string{pointer},
            "Select an online effective service CPU on a different physical core")};
    }
    const auto parameter = [&](const std::string& name) {
        return parameter_path("workloads", workload, "parameters", name);
    };
    Json result{{"operation", kernel::packages::mapping_scope(kind)},
                {"service_cpu", service_cpu.value()}, {"service_core", service->core_id.value()},
                {"page_mode", nullptr}, {"base_page_bytes", nullptr}, {"page_bytes", 0U},
                {"pages", 0U}, {"linux_numa_node", nullptr}};
    if (kind == MappingResource::allocation) {
        if (!parameters.contains("requested_bytes") ||
            !natural(parameters.at("requested_bytes"), 1U, UINT64_MAX) ||
            !kernel::packages::valid_allocation_extent(core::ByteCount::from_base_units(
                parameters.at("requested_bytes").get<std::uint64_t>()))) {
            if (related_settings) related_settings->assign({parameter("requested_bytes")});
            return std::unexpected{invalid("/workloads", "Select 1 byte through 64 MiB for the mapping request")};
        }
        result["requested_bytes"] = parameters.at("requested_bytes");
        return result;
    }
    const auto page_mode = parameters.at("page_mode");
    const auto base = core::ByteCount::from_base_units(coordinate.at("base_page_bytes").get<std::uint64_t>());
    const auto page = first_touch_page_size(machine, page_mode, base);
    if (!page) {
        if (related_settings) related_settings->assign({parameter("page_mode")});
        return std::unexpected{invalid("/workloads", "Selected first-touch page mode is unavailable")};
    }
    const auto& pages = parameters.at("pages");
    const auto extent = natural(pages, 1U, kernel::packages::maximum_first_touch_pages)
        ? kernel::packages::first_touch_extent(*page, pages.get<std::uint64_t>()) : std::nullopt;
    if (!extent) {
        if (related_settings) related_settings->assign({parameter("page_mode"), parameter("pages")});
        return std::unexpected{invalid("/workloads", "First-touch geometry exceeds the page or byte bounds")};
    }
    const auto worker = std::ranges::find(units, core::LinuxCpuId{coordinate.at("cpu").get<std::uint32_t>()},
                                          &topology::ProcessingUnit::linux_cpu_id);
    const auto local = topology::unique_local_memory_node(context.topology, context.stable_ids, worker->id, *page);
    if (!local || coordinate.at("memory_node") != local->linux_id.value()) {
        if (related_settings) {
            related_settings->assign({parameter("page_mode"), "/roles/0/cpus"});
            if (local)
                related_settings->push_back(role.contains("numa_nodes") ? "/roles/0/numa_nodes" : "/roles/0/numa_node");
        }
        return std::unexpected{invalid("/roles/0/numa_node", "Select the worker's exact local node with the requested page size")};
    }
    result.update(Json{{"page_mode", page_mode}, {"base_page_bytes", base.value()}, {"page_bytes", page->value()},
                       {"pages", pages}, {"linux_numa_node", local->linux_id.value()}, {"requested_bytes", extent->value()}});
    return result;
}

auto occupied_physical_cores(const std::set<core::LinuxCpuId>& occupied,
                             const ResourceResolutionContext& context) -> std::set<core::CoreId> {
    std::set<core::CoreId> cores;
    for (const auto& unit : context.topology.processing_units())
        if (occupied.contains(unit.linux_cpu_id))
            cores.insert(unit.core_id);
    return cores;
}

auto controller_representatives(const std::set<core::CoreId>& occupied,
                                const ResourceResolutionContext& context)
    -> std::vector<PackageControllerPlacement> {
    const auto& machine = context.topology.machine();
    const auto& effective = machine.effective_cpus.value();
    if (!effective)
        return {};

    std::map<core::CoreId, core::LinuxCpuId> representatives;
    for (const auto& unit : context.topology.processing_units()) {
        const auto cpu = unit.linux_cpu_id;
        if (occupied.contains(unit.core_id) || !machine.online_cpus.contains(cpu) ||
            !effective->contains(cpu))
            continue;
        const auto [position, inserted] = representatives.try_emplace(unit.core_id, cpu);
        if (!inserted && cpu < position->second)
            position->second = cpu;
    }
    std::vector<PackageControllerPlacement> result;
    result.reserve(representatives.size());
    for (const auto& [core, cpu] : representatives)
        if (const auto stable = context.stable_ids.core_id(core))
            result.push_back({stable->get(), cpu});
    std::ranges::sort(result, {}, &PackageControllerPlacement::cpu);
    return result;
}

void collect_role_cpus(const Json& source, std::set<core::LinuxCpuId>& occupied) {
    const auto roles = source.find("roles");
    if (roles == source.end() || !roles->is_array())
        return;
    for (const auto& role : *roles) {
        if (const auto cpus = role.find("cpus"); cpus != role.end() && cpus->is_array())
            for (const auto& cpu : *cpus)
                collect_package_occupied_cpu(cpu, occupied);
        if (const auto service = role.find("service_cpu"); service != role.end())
            collect_package_occupied_cpu(*service, occupied);
    }
}
} // namespace

auto first_touch_page_size(const topology::Machine& machine, const Json& mode, core::ByteCount base)
    -> std::optional<core::ByteCount> {
    if (mode == "base") return base;
    const auto huge = core::ByteCount::from_base_units(kernel::packages::huge_page_bytes);
    if (mode == "hugetlb-2m" && base.value() == 4096U && std::ranges::contains(machine.page_sizes, huge))
        return huge;
    return {};
}

void collect_package_occupied_cpu(const Json& value, std::set<core::LinuxCpuId>& occupied) {
    if (natural(value, 0U, UINT32_MAX))
        occupied.insert(core::LinuxCpuId{value.get<std::uint32_t>()});
}

void collect_package_occupied_cpus(const Json& source, std::set<core::LinuxCpuId>& occupied) {
    if (is_package_case_group(source)) {
        if (const auto cases = source.find("cases"); cases != source.end() && cases->is_array())
            for (const auto& one : *cases)
                collect_role_cpus(one, occupied);
    } else {
        collect_role_cpus(source, occupied);
    }
    if (const auto execution = source.find("execution"); execution != source.end())
        if (const auto monitoring = execution->find("monitoring"); monitoring != execution->end())
            if (const auto service = monitoring->find("service_cpu"); service != monitoring->end())
                collect_package_occupied_cpu(*service, occupied);
}

auto choose_package_controller(const Json& execution, const std::set<core::LinuxCpuId>& occupied,
                               const ResourceResolutionContext& context)
    -> std::expected<PackageControllerPlacement, core::Error> {
    const std::string path = "/execution/controller_core";
    const auto cores = occupied_physical_cores(occupied, context);
    const auto choices = controller_representatives(cores, context);
    const auto setting = execution.find("controller_core");
    if (setting == execution.end()) {
        if (choices.empty())
            return std::unexpected{invalid(path, "No spare physical core has an online effective controller CPU")};
        return choices.front();
    }
    if (!setting->is_string())
        return std::unexpected{invalid(path, "Expected a stable physical topology core ID")};
    const auto stable = topology::StableCoreId::parse(setting->get_ref<const std::string&>());
    if (!stable)
        return std::unexpected{invalid(path, "Expected a stable physical topology core ID")};
    const auto core = context.stable_ids.core(*stable);
    if (!core)
        return std::unexpected{invalid(path, "Controller core is absent from the topology")};
    if (cores.contains(*core)) {
        auto conflict = invalid(path, "Controller core is used by a worker or dedicated service CPU");
        conflict.context["controller_core"] = stable->value();
        auto& cpus = conflict.context["conflicting_cpus"] = Json::array();
        for (const auto& unit : context.topology.processing_units())
            if (unit.core_id == *core && occupied.contains(unit.linux_cpu_id))
                cpus.push_back(unit.linux_cpu_id.value());
        return std::unexpected{std::move(conflict)};
    }
    const auto selected = std::ranges::find(choices, *stable, &PackageControllerPlacement::core);
    if (selected == choices.end())
        return std::unexpected{invalid(path, "Controller core has no online effective CPU")};
    return *selected;
}

auto package_controller_choices(const Json& source, const ResourceResolutionContext& context)
    -> std::vector<PackageControllerPlacement> {
    std::set<core::LinuxCpuId> occupied;
    collect_package_occupied_cpus(source, occupied);
    return controller_representatives(occupied_physical_cores(occupied, context), context);
}

auto expand_package_cases(const Json& source) -> std::expected<std::vector<Json>, core::Error> {
    if (!is_package_case_group(source) ||
        !fields(source, {"schema", "mode", "workloads", "cases", "execution", "output"}) ||
        !source.contains("schema") || source.at("schema") != package_scenario_schema ||
        !source.contains("workloads") || !source.at("workloads").is_object() ||
        source.at("workloads").size() != 1U || !source.contains("execution") ||
        !source.at("execution").is_object() || !source.contains("cases"))
        return std::unexpected{invalid("", "Expected one shared workload, execution settings and named cases")};
    const auto& cases = source.at("cases");
    const auto& workload = source.at("workloads").begin().value();
    if (!cases.is_array() || cases.empty() || cases.size() > maximum_package_cases)
        return std::unexpected{invalid("/cases", "Expected 1 to 4096 comparison cases")};
    if (!workload.is_object())
        return std::unexpected{invalid("/workloads", "Expected an object for the shared workload")};
    std::set<std::string> ids;
    std::vector<Json> leaves;
    leaves.reserve(cases.size());
    for (const auto& one : cases) {
        const auto path = "/cases/" + std::to_string(leaves.size());
        if (!fields(one, {"id", "roles", "parameters"}) || !one.contains("id") ||
            !one.at("id").is_string() || !one.contains("roles") ||
            !one.at("roles").is_array() || one.at("roles").size() != 2U)
            return std::unexpected{invalid(path, "Expected a case ID and two comparison roles")};
        const auto& id = one.at("id").get_ref<const std::string&>();
        if (!ScenarioIdentifier::parse(id) || !ids.insert(id).second)
            return std::unexpected{invalid(path + "/id", "Expected a unique Scenario identifier")};
        if (one.contains("parameters")) {
            const auto& parameters = one.at("parameters");
            if (!parameters.is_object() || !workload.contains("parameters") ||
                !workload.at("parameters").is_object())
                return std::unexpected{invalid(path + "/parameters", "Expected shared task parameter overrides")};
            for (const auto& setting : parameters.items())
                if (!workload.at("parameters").contains(setting.key()))
                    return std::unexpected{invalid(path + "/parameters", "A case can override only authored shared parameters")};
        }
        leaves.push_back(materialize_package_case(source, leaves.size()));
    }
    return leaves;
}

auto resolve_package_cases(const Json& source, const Catalog& catalog,
                           const ResourceResolutionContext& topology)
    -> std::expected<std::vector<ResolvedPackageCase>, core::Error> {
    auto leaves = expand_package_cases(source);
    if (!leaves) return std::unexpected{std::move(leaves.error())};
    std::set<core::LinuxCpuId> occupied;
    collect_package_occupied_cpus(source, occupied);
    const auto controller = choose_package_controller(source.at("execution"), occupied, topology);
    if (!controller)
        return std::unexpected{controller.error()};
    std::vector<ResolvedPackageCase> resolved;
    resolved.reserve(leaves->size());
    for (auto& leaf : *leaves) {
        leaf["execution"]["controller_core"] = controller->core.value();
        auto plan = resolve_package(leaf, catalog, topology);
        if (!plan) {
            auto reason = std::move(plan.error());
            reason.context["case_id"] = leaf.at("case_id");
            return std::unexpected{std::move(reason)};
        }
        resolved.push_back({std::move(leaf), std::move(*plan)});
    }
    return resolved;
}

namespace {
auto resolve_package_plan(const Json& authored, const Catalog& catalog,
                          const ResourceResolutionContext& topology, const Json* saved_geometry,
                          bool select_controller, std::vector<std::string>* related_settings = nullptr,
                          std::span<const std::string> sweep_axes = {})
    -> std::expected<PackagePlan, core::Error> {
    const auto related = [&](std::initializer_list<std::string> paths) {
        if (related_settings) related_settings->assign(paths);
    };
    try {
        if (!fields(authored, {"schema", "mode", "workloads", "roles", "execution", "output", "case_id"}) ||
            authored.value("schema", "") != package_scenario_schema)
            return std::unexpected{invalid(
                "", "Expected a package Scenario with workloads, roles and execution settings")};
        const auto mode = authored.at("mode").get<std::string>();
        if (authored.contains("case_id") &&
            (mode != "comparison" || !authored.at("case_id").is_string() ||
             !ScenarioIdentifier::parse(authored.at("case_id").get_ref<const std::string&>())))
            return std::unexpected{invalid("/case_id", "Expected a Scenario identifier on a comparison case")};
        const auto& roles = authored.at("roles");
        const auto& workloads = authored.at("workloads");
        if (!roles.is_array() || roles.empty() || roles.size() > 64U || !workloads.is_object() ||
            workloads.empty() || workloads.size() > roles.size())
            return std::unexpected{
                invalid("/roles", "Expected 1 to 64 roles and a referenced workload for each")};
        if (!((mode == "standalone" && roles.size() == 1U) ||
              (mode == "comparison" && roles.size() == 2U) ||
              ((mode == "mixed" || mode == "pipeline") && roles.size() >= 2U)))
            return std::unexpected{invalid("/mode", "Choose standalone (one role), comparison "
                                                    "(two), mixed or pipeline (at least two)")};
        if (mode == "comparison" && workloads.size() != 1U)
            return std::unexpected{invalid(
                "/workloads", "Comparison roles must use the same workload and task inputs")};
        auto effective = authored;
        effective.erase("output");
        if (authored.contains("output")) {
            const auto& output = authored.at("output");
            if (!fields(output, {"directory", "format"}) || !output.contains("directory") ||
                !output.at("directory").is_string() || output.value("format", "json") != "json")
                return std::unexpected{
                    invalid("/output", "Expected a Result directory and JSON format")};
        }
        const auto& execution = effective.at("execution");
        if (!fields(execution, {"repetitions", "blocks", "order_seed", "hardware_counters",
                                "pipeline_chunks", "calibration", "warmup", "precision",
                                "monitoring", "controller_core"}) ||
            !natural(execution.at("repetitions"), 1, 1000000000U) ||
            !natural(execution.at("blocks"), 1, 4096) ||
            (execution.contains("order_seed") &&
             !natural(execution.at("order_seed"), 0, UINT64_MAX)) ||
            (execution.contains("hardware_counters") &&
             !execution.at("hardware_counters").is_boolean())) {
            if (related_settings && execution.is_object()) {
                if (execution.contains("repetitions") && !natural(execution.at("repetitions"), 1, 1000000000U))
                    related_settings->push_back("/execution/repetitions");
                if (execution.contains("blocks") && !natural(execution.at("blocks"), 1, 4096))
                    related_settings->push_back("/execution/blocks");
                if (execution.contains("hardware_counters") && !execution.at("hardware_counters").is_boolean())
                    related_settings->push_back("/execution/hardware_counters");
            }
            return std::unexpected{
                invalid("/execution", "Expected positive repetitions and blocks, an optional "
                                      "unsigned order seed and boolean hardware counters")};
        }
        if (execution.contains("precision")) {
            auto& precision = effective["execution"]["precision"];
            const auto max_blocks = execution.at("blocks").get<std::uint64_t>();
            if (mode != "comparison" ||
                !fields(precision, {"minimum_blocks", "minimum_valid_blocks", "confidence",
                                    "relative_half_width"}))
                return std::unexpected{invalid("/execution/precision",
                                               "Ratio precision requires a comparison")};
            if (!precision.contains("minimum_blocks"))
                precision["minimum_blocks"] = std::uint64_t{8};
            if (!precision.contains("minimum_valid_blocks"))
                precision["minimum_valid_blocks"] = precision.at("minimum_blocks");
            if (!precision.contains("confidence"))
                precision["confidence"] = {{"numerator", std::uint64_t{19}},
                                           {"denominator", std::uint64_t{20}}};
            if (!precision.contains("relative_half_width"))
                precision["relative_half_width"] = {{"numerator", std::uint64_t{1}},
                                                    {"denominator", std::uint64_t{50}}};
            if (!natural(precision.at("minimum_blocks"), 2U, max_blocks) ||
                !natural(precision.at("minimum_valid_blocks"), 2U, max_blocks) ||
                !fraction(precision.at("confidence"), true) ||
                !fraction(precision.at("relative_half_width"), false)) {
                const auto exceeds_limit = [max_blocks](const Json& value) {
                    return value.is_number_unsigned() && value.get<std::uint64_t>() > max_blocks;
                };
                if (exceeds_limit(precision.at("minimum_blocks")) || exceeds_limit(precision.at("minimum_valid_blocks")))
                    related({"/execution/blocks"});
                return std::unexpected{invalid(
                    "/execution/precision", "Use a minimum of at least two paired blocks within "
                                            "the block limit, and exact confidence and width ratios")};
            }
        }
        if (execution.contains("monitoring") &&
            !normalize_monitoring(effective["execution"]["monitoring"]))
            return std::unexpected{invalid(
                "/execution/monitoring", "Use unique known required and optional channels, boolean "
                                         "sampling flags and an enabled during interval from 1 microsecond "
                                         "through 1 hour")};
        if ((execution.contains("calibration") || execution.contains("warmup")) &&
            mode != "standalone" && mode != "comparison")
            return std::unexpected{
                invalid("/execution",
                        "Adaptive observation controls require a standalone task or a comparison")};
        if (execution.contains("calibration")) {
            const auto& c = execution.at("calibration");
            if (!fields(c, {"minimum_ns", "target_ns", "maximum_ns", "max_probes"}) ||
                !natural(c.at("minimum_ns"), 1, UINT64_MAX) ||
                !natural(c.at("target_ns"), 1, UINT64_MAX) ||
                !natural(c.at("maximum_ns"), 1, UINT64_MAX) ||
                !natural(c.at("max_probes"), 1, 128) || c.at("target_ns") < c.at("minimum_ns") ||
                c.at("maximum_ns") < c.at("target_ns"))
                return std::unexpected{
                    invalid("/execution/calibration",
                            "Use an ordered positive duration band and 1 to 128 common probes")};
        }
        if (execution.contains("warmup")) {
            const auto& w = execution.at("warmup");
            if (!fields(w, {"minimum_trials", "maximum_trials", "stability_window",
                            "stability_numerator", "stability_denominator"}) ||
                !natural(w.at("minimum_trials"), 1, 64) ||
                !natural(w.at("maximum_trials"), 1, 64) ||
                !natural(w.at("stability_window"), 1, 64) ||
                !natural(w.at("stability_numerator"), 0, UINT64_MAX) ||
                !natural(w.at("stability_denominator"), 1, UINT64_MAX) ||
                w.at("minimum_trials") > w.at("maximum_trials") ||
                w.at("stability_window") > w.at("maximum_trials") ||
                w.at("stability_numerator") > w.at("stability_denominator"))
                return std::unexpected{invalid(
                    "/execution/warmup",
                    "Choose a finite warmup window and a stability fraction between zero and one")};
        }
        const bool streaming = execution.contains("pipeline_chunks");
        if (streaming && (mode != "pipeline" || roles.size() != 2U ||
                          !natural(execution.at("pipeline_chunks"), 2, 64) ||
                          execution.at("repetitions").get<std::uint64_t>() >
                              64U / execution.at("pipeline_chunks").get<std::uint64_t>() ||
                          execution.value("hardware_counters", false))) {
            if (related_settings) {
                const auto& chunks = execution.at("pipeline_chunks");
                if (!natural(chunks, 2, 64))
                    related({"/execution/pipeline_chunks"});
                else if (execution.at("repetitions").get<std::uint64_t>() > 64U / chunks.get<std::uint64_t>())
                    related({"/execution/pipeline_chunks", "/execution/repetitions"});
                if (execution.value("hardware_counters", false))
                    related_settings->push_back("/execution/hardware_counters");
            }
            return std::unexpected{
                invalid("/execution/pipeline_chunks",
                        "Streaming requires two pipeline stages, 2 to 64 chunks, at most 64 chunk "
                        "calls per stage and no worker hardware counters")};
        }
        effective["execution"]["hardware_counters"] = execution.value("hardware_counters", false);
        const auto& machine = topology.topology.machine();
        const auto& effective_cpus = machine.effective_cpus.value();
        const auto& effective_nodes = machine.effective_nodes.value();
        const auto page_bytes =
            machine.page_sizes.empty() ? 0U : std::ranges::min(machine.page_sizes).value();
        std::set<std::string> role_ids, used_workloads;
        std::set<std::uint32_t> concurrent_cpus;
        Json bindings = Json::object(), inputs = Json::object(), placements = Json::array(),
             composition = Json::array(), mapping = nullptr;
        const kernel::packages::ImplementationManifest* previous{};
        for (std::size_t i = 0; i < roles.size(); ++i) {
            const auto& role = roles.at(i);
            const auto index = std::to_string(i);
            const std::string pointer = "/roles/" + index;
            if (!fields(role, {"id", "workload", "implementation", "tuning", "cpus", "numa_node",
                               "numa_nodes", "inputs", "input_placement", "service_cpu"}))
                return std::unexpected{invalid(pointer, "Unexpected role setting")};
            const auto id = role.at("id").get<std::string>();
            const auto workload_id = role.at("workload").get<std::string>();
            if (!ScenarioIdentifier::parse(id) || !role_ids.insert(id).second ||
                !ScenarioIdentifier::parse(workload_id) || !workloads.contains(workload_id))
                return std::unexpected{
                    invalid(pointer, "Role IDs must be unique and reference a defined workload")};
            const auto& workload = workloads.at(workload_id);
            if (!fields(workload, {"task", "parameters"}))
                return std::unexpected{
                    invalid("/workloads/" + workload_id, "Unexpected workload setting")};
            const auto task = workload.at("task").get<std::string>();
            const auto implementation = role.at("implementation").get<std::string>();
            const auto* module = catalog.find(task, implementation);
            if (!module)
                return std::unexpected{
                    invalid(pointer + "/implementation",
                            "No installed implementation " + implementation + " for " + task)};
            const auto resource = module->task->mapping;
            const bool mapping_role = resource != MappingResource::none;
            const bool allocation = resource == MappingResource::allocation;
            if (mapping_role && (mode != "standalone" || execution.at("repetitions") != 1 ||
                execution.contains("calibration") || execution.contains("warmup") ||
                execution.at("hardware_counters") == true ||
                (allocation ? role.contains("input_placement") :
                 role.value("input_placement", Json{{"policy", "bind"}}) != Json{{"policy", "bind"}})))
                return std::unexpected{invalid(pointer, "Fresh mappings require standalone execution with one repetition per block and no adaptive controls or input policies")};
            if (!mapping_role && role.contains("service_cpu"))
                return std::unexpected{invalid(pointer + "/service_cpu", "Only a fresh mapping uses a dedicated service CPU")};
            if (execution.contains("warmup") && module->task->collective) {
                related({pointer + "/implementation"});
                return std::unexpected{invalid(
                    "/execution/warmup",
                    "This task uses mutable collective state without a warmup reset contract")};
            }
            if (streaming &&
                (role.at("cpus").size() != 1U || module->task->collective ||
                 (i == 1U && (!role.contains("inputs") || role.at("inputs").size() != 1U)))) {
                if (related_settings && role.at("cpus").size() != 1U)
                    related_settings->push_back(pointer + "/cpus");
                if (related_settings && module->task->collective)
                    related_settings->push_back(pointer + "/implementation");
                return std::unexpected{invalid(
                    pointer, "Streaming requires one rank per stage and exactly one named link")};
            }
            if (mode == "pipeline" && previous) {
                if (!role.contains("inputs") || !role.at("inputs").is_object() ||
                    role.at("inputs").empty() ||
                    role.at("cpus").size() != roles.at(i - 1).at("cpus").size()) {
                    if (related_settings && role.contains("cpus") &&
                        role.at("cpus").size() != roles.at(i - 1).at("cpus").size())
                        related({"/roles/" + std::to_string(i - 1) + "/cpus", pointer + "/cpus"});
                    return std::unexpected{
                        invalid(pointer + "/inputs", "A stage needs named links to the preceding "
                                                     "stage and the same worker count")};
                }
                for (const auto& [input, output] : role.at("inputs").items()) {
                    const auto destination = module->task->ports.inputs.find(input);
                    if (destination == module->task->ports.inputs.end() || !output.is_string())
                        return std::unexpected{
                            invalid(pointer + "/inputs",
                                    "Unknown destination input or invalid source port")};
                    const auto origin =
                        previous->task->ports.outputs.find(output.get_ref<const std::string&>());
                    if (origin == previous->task->ports.outputs.end() ||
                        origin->second != destination->second)
                        return std::unexpected{invalid(
                            pointer + "/inputs",
                            "Source and destination ports must exist and have identical formats")};
                }
            } else if (role.contains("inputs")) {
                return std::unexpected{
                    invalid(pointer + "/inputs", "Only a successor pipeline stage accepts links")};
            }
            previous = module;
            auto parameters = kernel::packages::resolve_parameters(
                module->task->parameters, workload.value("parameters", Json::object()),
                "/workloads/" + workload_id + "/parameters");
            if (!parameters) {
                if (related_settings) {
                    const auto parent = "/workloads/" + workload_id + "/parameters";
                    const auto field = parameters.error().context.value("field", std::string{});
                    if (field.starts_with(parent + "."))
                        related({parameter_path("workloads", workload_id, "parameters", field.substr(parent.size() + 1U))});
                }
                return std::unexpected{parameters.error()};
            }
            std::string rejected_parameter;
            if (auto supported = kernel::packages::validate_implementation_inputs(
                    module->accepts, *parameters, pointer + "/implementation",
                    related_settings ? &rejected_parameter : nullptr);
                !supported) {
                if (related_settings)
                    related({pointer + "/implementation",
                             parameter_path("workloads", workload_id, "parameters", rejected_parameter)});
                return std::unexpected{supported.error()};
            }
            auto tuning = kernel::packages::resolve_parameters(
                module->tuning, role.value("tuning", Json::object()), pointer + "/tuning");
            if (!tuning) {
                if (related_settings) {
                    const auto parent = pointer + "/tuning";
                    const auto field = tuning.error().context.value("field", std::string{});
                    if (field.starts_with(parent + ".")) {
                        const auto name = field.substr(parent.size() + 1U);
                        related({parameter_path("roles", index, "tuning", name)});
                        if (!module->tuning.contains(name))
                            related_settings->push_back(pointer + "/implementation");
                    }
                }
                return std::unexpected{tuning.error()};
            }
            effective["workloads"][workload_id]["parameters"] = *parameters;
            effective["roles"][i]["tuning"] = std::move(*tuning);
            used_workloads.insert(workload_id);
            if (!inputs.contains(workload_id))
                inputs[workload_id] = {{"task", task},
                                       {"parameters", *parameters},
                                       {"task_manifest_sha256", module->task->sha256.to_hex()},
                                       {"oracle_sha256", module->task->oracle_sha256.to_hex()}};
            bindings[id] = {{"task_manifest", module->task->contents},
                            {"task_manifest_sha256", module->task->sha256.to_hex()},
                            {"oracle_sha256", module->task->oracle_sha256.to_hex()},
                            {"implementation_manifest", module->contents},
                            {"implementation_manifest_sha256", module->sha256.to_hex()},
                            {"module_sha256", module->module_sha256.to_hex()}};
            const auto& cpus = role.at("cpus");
            if (module->task->per_worker_input)
                inputs[workload_id]["workers_by_role"][id] = cpus.size();
            if (!cpus.is_array() || cpus.empty() || cpus.size() > 1024U ||
                (allocation ? role.contains("numa_node") || role.contains("numa_nodes") :
                 role.contains("numa_node") == role.contains("numa_nodes"))) {
                if (!cpus.is_array() || cpus.empty() || cpus.size() > 1024U)
                    related({pointer + "/cpus"});
                return std::unexpected{invalid(pointer, "Choose worker CPUs and either one NUMA "
                                                        "node or an ordered per-rank node list for placed memory")};
            }
            if (mode == "comparison" && module->task->per_worker_input && i > 0 &&
                roles.front().at("cpus").size() != cpus.size()) {
                related({"/roles/0/cpus", pointer + "/cpus"});
                return std::unexpected{
                    invalid(pointer + "/cpus", "This task defines its input per worker. "
                                               "Comparisons require equal worker counts.")};
            }
            if (cpus.size() < module->task->ranks.minimum ||
                cpus.size() > module->task->ranks.maximum)
                return std::unexpected{invalid(
                    pointer + "/cpus", "Worker count is outside the task manifest rank bounds")};
            Json memory_nodes = Json::array();
            if (allocation) {
                memory_nodes.push_back(nullptr);
            } else if (role.contains("numa_node")) {
                for (std::size_t rank = 0; rank < cpus.size(); ++rank)
                    memory_nodes.push_back(role.at("numa_node"));
            } else
                memory_nodes = role.at("numa_nodes");
            if (!memory_nodes.is_array() || memory_nodes.size() != cpus.size()) {
                if (memory_nodes.is_array()) related({pointer + "/numa_nodes", pointer + "/cpus"});
                return std::unexpected{invalid(pointer + "/numa_nodes",
                                               "Provide one NUMA node per selected worker CPU")};
            }
            const auto& nodes = topology.topology.numa_nodes();
            const auto usable_node = [&](const Json& node) {
                if (!natural(node, 0, UINT32_MAX))
                    return false;
                const auto linux_node = node.get<std::uint32_t>();
                const auto usable = std::ranges::find_if(nodes, [&](const auto& item) {
                    return item.linux_node_id.value() == linux_node &&
                           item.installed_memory.value() &&
                           item.installed_memory.value()->value() != 0;
                });
                return usable != nodes.end() && effective_nodes &&
                       effective_nodes->contains(core::NumaNodeId{linux_node}) &&
                       machine.online_nodes.contains(core::NumaNodeId{linux_node});
            };
            if (!allocation && !std::ranges::all_of(memory_nodes, usable_node)) {
                related({pointer + (role.contains("numa_node") ? "/numa_node" : "/numa_nodes")});
                return std::unexpected{
                    invalid(pointer, "NUMA node must have memory and be online and effective")};
            }
            if (!allocation)
                effective["roles"][i]["input_placement"] =
                    role.value("input_placement", Json{{"policy", "bind"}});
            if (role.contains("input_placement")) {
                const auto& placement = role.at("input_placement");
                const auto field = pointer + "/input_placement";
                if (!fields(placement, {"policy", "nodes", "replica_group"}) ||
                    !placement.contains("policy"))
                    return std::unexpected{invalid(field, "Expected an input placement policy")};
                const auto policy = placement.at("policy");
                if (policy == "bind" || policy == "interleave") {
                    if (placement.contains("replica_group") ||
                        (policy == "interleave" && !placement.contains("nodes")))
                        return std::unexpected{
                            invalid(field, "Choose explicit memory nodes for interleave and "
                                           "reserve replica_group for replicas")};
                    if (placement.contains("nodes")) {
                        const auto& targets = placement.at("nodes");
                        if (!targets.is_array() ||
                            targets.size() < (policy == "interleave" ? 2U : 1U) ||
                            targets.size() > 1024U || !std::ranges::all_of(targets, usable_node) ||
                            std::set<Json>(targets.begin(), targets.end()).size() != targets.size())
                            return std::unexpected{
                                invalid(field, "Input nodes must be distinct and usable; "
                                               "interleave requires at least two")};
                    }
                } else if (policy == "replica_per_worker" || policy == "replica_per_node") {
                    if (placement.contains("nodes") || !placement.contains("replica_group") ||
                        !placement.at("replica_group").is_string() ||
                        !ScenarioIdentifier::parse(
                            placement.at("replica_group").get<std::string>()))
                        return std::unexpected{
                            invalid(field, "Read-only replicas require a named replica_group and "
                                           "use the worker memory nodes")};
                } else
                    return std::unexpected{invalid(field, "Unknown input placement policy")};
            }
            std::set<std::uint32_t> ranks;
            Json cores = Json::array(), locality = Json::array();
            for (const auto& item : cpus) {
                if (!natural(item, 0, UINT32_MAX))
                    return std::unexpected{invalid(pointer + "/cpus", "Invalid Linux CPU")};
                const auto cpu = item.get<std::uint32_t>();
                const auto& pus = topology.topology.processing_units();
                const auto pu = std::ranges::find_if(
                    pus, [cpu](const auto& unit) { return unit.linux_cpu_id.value() == cpu; });
                const bool concurrent = mode == "mixed" || streaming;
                if (pu == pus.end() || !effective_cpus ||
                    !effective_cpus->contains(core::LinuxCpuId{cpu}) ||
                    !machine.online_cpus.contains(core::LinuxCpuId{cpu}) ||
                    !ranks.insert(cpu).second ||
                    (concurrent && !concurrent_cpus.insert(cpu).second)) {
                    if (related_settings && concurrent && concurrent_cpus.contains(cpu)) {
                        related({pointer + "/cpus"});
                        for (std::size_t earlier = 0; earlier < i; ++earlier)
                            if (std::ranges::contains(roles.at(earlier).at("cpus"), item))
                                related_settings->push_back("/roles/" + std::to_string(earlier) + "/cpus");
                    }
                    return std::unexpected{invalid(
                        pointer + "/cpus",
                        "Worker CPUs must be online, effective and distinct for concurrent roles")};
                }
                Json coordinate{{"cpu", cpu},
                                {"core", pu->core_id.value()},
                                {"linux_numa_node", nullptr},
                                {"memory_node", memory_nodes.at(locality.size())},
                                {"package", nullptr},
                                {"llc", nullptr},
                                {"base_page_bytes", page_bytes},
                                {"worker_id", nullptr},
                                {"l1", nullptr}};
                const auto local = topology::unique_local_memory_node(
                    topology.topology, topology.stable_ids, pu->id,
                    core::ByteCount::from_base_units(page_bytes));
                if (local)
                    coordinate["linux_numa_node"] = local->linux_id.value();
                if (const auto worker = topology.stable_ids.processing_unit_id(pu->id)) {
                    coordinate["worker_id"] = worker->get().value();
                    if (const auto cache = topology::unique_l1_data_geometry(
                            topology.topology, topology.stable_ids, worker->get()))
                        coordinate["l1"] = {{"id", cache->cache_id},
                                            {"capacity_bytes", cache->capacity_bytes},
                                            {"line_bytes", cache->line_bytes},
                                            {"ways", cache->ways}};
                }
                const auto& cores_in_machine = topology.topology.cores();
                const auto core =
                    std::ranges::find(cores_in_machine, pu->core_id, &topology::Core::id);
                if (core != cores_in_machine.end())
                    coordinate["package"] = core->package_id.value();
                if (const auto llc = topology::highest_unique_unified_cache(
                        topology.topology, topology.stable_ids, pu->id))
                    if (const auto cache_id = topology.stable_ids.cache(llc->id))
                        coordinate["llc"] = cache_id->value();
                locality.push_back(std::move(coordinate));
                cores.push_back(pu->core_id.value());
            }
            if (module->task->ranks.distinct_cores &&
                std::set<Json>(cores.begin(), cores.end()).size() != cpus.size())
                return std::unexpected{invalid(
                    pointer + "/cpus", "The task manifest requires distinct physical cores")};
            if (module->task->shared_cpu_local) {
                const auto& local = locality.at(0).at("linux_numa_node");
                if (local.is_null() || !effective_nodes ||
                    !effective_nodes->contains(core::NumaNodeId{local.get<std::uint32_t>()}) ||
                    !std::ranges::any_of(nodes, [&](const auto& n) {
                        return n.linux_node_id.value() == local.get<std::uint32_t>() &&
                               n.installed_memory.value() &&
                               n.installed_memory.value()->value() != 0;
                    }))
                    return std::unexpected{invalid(
                        pointer,
                        "Shared workspace needs an effective CPU-local memory node for its owner")};
            }
            Json placement{{"role", id},
                           {"cpus", cpus},
                           {"cores", cores},
                           {"locality", locality},
                           {"numa_nodes", memory_nodes}};
            if (role.contains("numa_node"))
                placement["numa_node"] = role.at("numa_node");
            if (mapping_role) {
                auto admitted = mapping_plan(resource, role, *parameters, topology, locality.at(0), workload_id,
                                             related_settings);
                if (!admitted) return std::unexpected{admitted.error()};
                mapping = std::move(*admitted);
            }
            placements.push_back(std::move(placement));
            composition.push_back(
                {{"workload", workload_id}, {"links", role.value("inputs", Json::object())}});
        }
        if (used_workloads.size() != workloads.size())
            return std::unexpected{invalid("/workloads", "Each declared workload must be used")};
        if (execution.contains("monitoring") && execution.at("monitoring").contains("service_cpu")) {
            const core::LinuxCpuId cpu{execution.at("monitoring").at("service_cpu").get<std::uint32_t>()};
            const auto units = topology.topology.processing_units();
            const auto service = std::ranges::find(units, cpu, &topology::ProcessingUnit::linux_cpu_id);
            if (service == units.end() || !effective_cpus || !effective_cpus->contains(cpu) ||
                !machine.online_cpus.contains(cpu) ||
                (!mapping.is_null() && mapping.at("service_core") == service->core_id.value()) ||
                std::ranges::any_of(placements, [&](const Json& placement) {
                    return std::ranges::contains(placement.at("cores"), Json(service->core_id.value()));
                }))
                return std::unexpected{invalid("/execution/monitoring/service_cpu",
                     "Use an online effective CPU on a physical core separate from workers and mapping service")};
        }
        std::optional<PackageControllerPlacement> controller;
        if (select_controller) {
            std::set<core::LinuxCpuId> occupied;
            collect_package_occupied_cpus(authored, occupied);
            auto selected = choose_package_controller(execution, occupied, topology);
            if (!selected)
                return std::unexpected{std::move(selected.error())};
            controller.emplace(std::move(*selected));
        }
        auto metric_scope = scope(mode, streaming);
        if (!mapping.is_null())
            metric_scope = kernel::packages::mapping_scope(previous->task->mapping);
        if ((mode == "standalone" || mode == "comparison") && previous->task->collective)
            metric_scope = previous->task->collective_finalizer ? "collective-release-to-completion"
                                                                : "collective-worker-wall-time";
        Json evidence{{"scenario", effective},
                      {"bindings", bindings},
                      {"placement", placements},
                      {"scope", metric_scope},
                      {"physical_topology", topology.fingerprints.physical.to_string()},
                      {"effective_topology", topology.fingerprints.effective.to_string()}};
        if (controller)
            evidence["controller"] = {{"core", controller->core.value()}, {"cpu", controller->cpu.value()}};
        if (mode == "mixed")
            evidence["mixed_protocol"] = package_mixed_protocol;
        if (!mapping.is_null())
            evidence["mapping"] = std::move(mapping);
        if (saved_geometry) {
            if (!saved_geometry->is_array() || saved_geometry->empty())
                return std::unexpected{invalid("/roles", "Retained layout is missing")};
            if (evidence.contains("mapping") && *saved_geometry != kernel::packages::mapping_geometry(roles.at(0)))
                return std::unexpected{invalid("/roles", "Retained fresh mapping geometry differs")};
            evidence["geometry"] = *saved_geometry;
        } else if (catalog.inspect_layouts) {
            auto geometry = catalog.inspect_layouts(catalog, evidence, page_bytes);
            if (!geometry) {
                auto error = std::move(geometry.error());
                const auto paths = error.context.find(kernel::packages::layout_related_settings);
                if (paths != error.context.end()) {
                    if (related_settings && paths->is_array()) {
                        for (const auto& entry : *paths) {
                            if (!entry.is_string()) continue;
                            const auto& path = entry.get_ref<const std::string&>();
                            if (std::ranges::contains(sweep_axes, path))
                                related_settings->push_back(path);
                        }
                    }
                    error.context.erase(paths);
                }
                return std::unexpected{std::move(error)};
            }
            evidence["geometry"] = std::move(*geometry);
        } else
            return std::unexpected{
                invalid("/roles", "Installed package layout inspection is required")};
        evidence["linux_packages"] = Json::array();
        for (const auto& package : topology.topology.packages())
            if (package.os_id.value())
                evidence["linux_packages"].push_back(*package.os_id.value());
        const auto input = digest(inputs);
        Json problem_scope{{"inputs", inputs},
                           {"mode", mode},
                           {"composition", composition},
                           {"scope", metric_scope}};
        auto conditions = effective.at("execution");
        conditions.erase("order_seed");
        conditions.erase("blocks");
        conditions.erase("controller_core");
        if (streaming) conditions.erase("pipeline_chunks");
        problem_scope["measurement"] = std::move(conditions);
        problem_scope["physical_topology"] = evidence.at("physical_topology");
        problem_scope["effective_topology"] = evidence.at("effective_topology");
        if (controller)
            problem_scope["controller"] = evidence.at("controller");
        if (evidence.contains("mixed_protocol"))
            problem_scope["mixed_protocol"] = evidence.at("mixed_protocol");
        // Ratios from different references do not describe the same improvement.
        if (mode == "comparison") {
            const auto& reference = effective.at("roles").front();
            problem_scope["reference"] = reference;
            problem_scope["reference_binding"] = bindings.at(reference.at("id").get<std::string>());
        }
        const auto problem = digest(problem_scope);
        auto candidate = evidence;
        candidate["scenario"]["execution"].erase("order_seed");
        candidate["scenario"]["execution"].erase("controller_core");
        return PackagePlan{evidence, digest(effective), digest(evidence),
                           input,    problem,           digest(candidate), std::move(controller)};
    } catch (const Json::exception& e) {
        return std::unexpected{invalid("", std::string("Malformed package Scenario: ") + e.what())};
    }
}
} // namespace

auto resolve_package(const Json& authored, const Catalog& catalog,
                     const ResourceResolutionContext& topology, std::vector<std::string>* related_settings,
                     std::span<const std::string> sweep_axes)
    -> std::expected<PackagePlan, core::Error> {
    return resolve_package_plan(authored, catalog, topology, nullptr, true, related_settings, sweep_axes);
}

auto replay_package(const Json& authored, const Catalog& catalog,
                    const ResourceResolutionContext& topology, const Json& saved_plan)
    -> std::expected<PackagePlan, core::Error> {
    const bool has_controller = saved_plan.contains("controller");
    if (!has_controller && authored.contains("execution") &&
        authored.at("execution").contains("controller_core"))
        return std::unexpected{invalid("/execution/controller_core",
                                       "Retained plan is missing its authored controller")};
    return resolve_package_plan(authored, catalog, topology, &saved_plan.at("geometry"), has_controller);
}

auto validate_package_sweep_base(const Json& authored, const Catalog& catalog,
                                 const ResourceResolutionContext& topology)
    -> std::expected<Json, core::Error> {
    auto resolved = resolve_package_plan(authored, catalog, topology, nullptr, false);
    if (!resolved)
        return std::unexpected{std::move(resolved.error())};
    return std::move(resolved->evidence.at("scenario"));
}

bool is_package_input_axis(std::string_view path) noexcept {
    return path.starts_with("/workloads/");
}

auto package_axes(const Json& base, const Catalog& catalog)
    -> std::expected<std::vector<std::string>, core::Error> {
    if (is_package_case_group(base)) {
        auto leaves = expand_package_cases(base);
        if (!leaves) return std::unexpected{leaves.error()};
        auto common = package_axes(leaves->front(), catalog);
        if (!common) return common;
        std::erase_if(*common, [&](const std::string& path) {
            if (path.starts_with("/execution/")) return false;
            if (!is_package_input_axis(path)) return true;
            const auto parameter = Json::json_pointer(path).back();
            return std::ranges::any_of(base.at("cases"), [&](const auto& one) {
                return one.contains("parameters") && one.at("parameters").contains(parameter);
            });
        });
        return common;
    }
    std::vector<std::string> axes;
    bool mapping = false;
    std::set<std::string> seen_workloads;
    try {
        for (std::size_t i = 0; i < base.at("roles").size(); ++i) {
            const auto& role = base.at("roles")[i];
            const auto workload_id = role.at("workload").get<std::string>();
            const auto& workload = base.at("workloads").at(workload_id);
            const auto* impl = catalog.find(workload.at("task").get<std::string>(),
                                            role.at("implementation").get<std::string>());
            if (!impl)
                return std::unexpected{invalid("/roles", "Unknown task or implementation")};
            mapping |= impl->task->mapping != MappingResource::none;
            if (seen_workloads.insert(workload_id).second)
                for (const auto& entry : impl->task->parameters)
                    axes.push_back(
                        parameter_path("workloads", workload_id, "parameters", entry.first));
            for (const auto& entry : impl->tuning)
                axes.push_back(parameter_path("roles", std::to_string(i), "tuning", entry.first));
            axes.push_back("/roles/" + std::to_string(i) + "/cpus");
            if (impl->task->mapping != MappingResource::allocation)
                axes.push_back("/roles/" + std::to_string(i) +
                               (role.contains("numa_nodes") ? "/numa_nodes" : "/numa_node"));
            if (impl->task->mapping != MappingResource::none)
                axes.push_back("/roles/" + std::to_string(i) + "/service_cpu");
        }
    } catch (const Json::exception& e) {
        return std::unexpected{invalid("", e.what())};
    }
    if (!mapping)
        axes.emplace_back("/execution/repetitions");
    axes.emplace_back("/execution/blocks");
    if (!mapping && !has_stream_chunks(base))
        axes.emplace_back("/execution/hardware_counters");
    for (std::size_t i = 0; i < base.at("roles").size(); ++i)
        axes.push_back("/roles/" + std::to_string(i) + "/implementation");
    if (!mapping)
        for (std::size_t i = 0; i < base.at("roles").size(); ++i)
            axes.push_back("/roles/" + std::to_string(i) + "/input_placement");
    if (has_stream_chunks(base))
        axes.emplace_back("/execution/pipeline_chunks");
    return axes;
}
} // namespace epycbench::scenario
