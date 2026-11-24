#include "cli/package_authoring.hpp"
#include "cli/run_output_policy.hpp"
#include "cli/scenario_guides.hpp"
#include "epycbench/scenario/configuration_space.hpp"
#include "epycbench/topology/worker_locality.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <istream>
#include <map>
#include <ostream>
#include <set>

namespace epycbench::cli {
namespace {
using Json = nlohmann::json;
using namespace kernel::packages;
using namespace scenario_guides;
using namespace scenario_topology;
using ControllerChoices = std::vector<scenario::PackageControllerPlacement>;

constexpr std::string_view bandwidth_task = "memory.bandwidth.read/v1";
constexpr std::string_view bandwidth_implementation = "scalar-u64-sequential/v1";
constexpr std::string_view latency_task = "memory.latency.dependent-load/v1";
constexpr std::string_view latency_implementation = "scalar-u64-random-cycle/v1";

struct Prompts {
    std::istream& in;
    std::ostream& out;
    std::optional<std::string> line(std::string_view label) {
        out << label << "> " << std::flush;
        std::string answer;
        if (!std::getline(in, answer) || !out || answer == "q" || answer == "quit")
            return {};
        return answer;
    }
    std::optional<std::size_t> number(std::string_view label, std::size_t low, std::size_t high,
                                      std::optional<std::size_t> fallback = {}) {
        while (auto answer = line(label)) {
            if (answer->empty() && fallback)
                return fallback;
            std::size_t n{};
            const auto result = std::from_chars(answer->data(), answer->data() + answer->size(), n);
            if (result.ec == std::errc{} && result.ptr == answer->data() + answer->size() &&
                n >= low && n <= high)
                return n;
            out << "Enter a number from " << low << " to " << high << ".\n";
        }
        return {};
    }
    std::optional<Json> indices(std::string_view label, bool memory = false) {
        while (auto answer = line(label)) {
            auto value = Json::parse(*answer, nullptr, false);
            if (memory && value.is_object() && value.contains("input_placement") &&
                value.size() == 2U && (value.contains("numa_node") || value.contains("numa_nodes")))
                return value;
            if (value.is_number_unsigned() && value.get<std::uint64_t>() <= UINT32_MAX)
                return value;
            if (value.is_array() && !value.empty() && value.size() <= 1024U &&
                std::ranges::all_of(value, [](const Json& cpu) {
                    return cpu.is_number_unsigned() && cpu.get<std::uint64_t>() <= UINT32_MAX;
                }))
                return value;
            out << "Enter an unsigned index or a nonempty JSON array of indices.\n";
        }
        return {};
    }
    std::optional<Json> settings(const ParameterDefinitions& definitions) {
        Json settings = Json::object();
        for (const auto& [name, spec] : definitions) {
            out << name;
            if (spec.minimum)
                out << " min " << *spec.minimum;
            if (spec.maximum)
                out << " max " << *spec.maximum;
            if (!spec.values.empty())
                out << " choices " << Json(spec.values);
            if (spec.type == ParameterType::boolean)
                out << " (true or false)";
            if (spec.default_value)
                out << " (Enter for " << *spec.default_value << ')';
            out << '\n';
            for (;;) {
                auto answer = line(name);
                if (!answer)
                    return {};
                Json value = Json::parse(*answer, nullptr, false);
                if (answer->empty() && spec.default_value)
                    value = *spec.default_value;
                else if (spec.type == ParameterType::enumeration && !value.is_string())
                    value = *answer;
                const ParameterDefinitions field{{name, spec}};
                if (!value.is_discarded()) {
                    auto result = resolve_parameters(field, Json{{name, value}}, name);
                    if (result) {
                        settings[name] = result->at(name);
                        break;
                    }
                    out << result.error().message << '\n';
                } else
                    out << "Enter a value of the declared type.\n";
            }
        }
        return settings;
    }
};

int report(const core::Error& failure, std::ostream& out) {
    out << failure.code.value() << ' ' << failure.message << ' ' << failure.context.dump() << '\n';
    return 6;
}

auto numa_matrix_sites(const scenario::ResourceResolutionContext& context, const Workers& workers,
                       const MemoryNodes& nodes) -> NumaMatrix {
    if (nodes.size() < 2U) return {};
    const auto page = std::ranges::min(context.topology.machine().page_sizes);
    constexpr std::uint64_t kib = 1024U;
    const auto capacity = (*std::ranges::min_element(nodes, {}, [](const auto* node) {
        return node->installed_memory.value()->value();
    }))->installed_memory.value()->value();
    NumaMatrix matrix{{}, nodes, std::max<std::uint64_t>(4U, (page.value() - 1U) / kib + 1U),
                      capacity / page.value() * page.value() / kib};
    for (const auto* worker : workers) {
        const auto local = topology::unique_local_memory_node(
            context.topology, context.stable_ids, worker->id, page);
        if (local)
            matrix.readers[local->linux_id.value()].push_back(worker);
    }
    if (matrix.maximum_kib < matrix.minimum_kib ||
        matrix.readers.size() > scenario::maximum_package_cases / (nodes.size() - 1U))
        return {};
    return matrix;
}

auto case_roles(std::string_view workload, std::string_view implementation, std::uint32_t cpu,
                std::uint32_t local, std::uint32_t remote) -> Json {
    Json baseline{{"id", "local"}, {"workload", workload}, {"implementation", implementation},
                  {"cpus", Json::array({cpu})}, {"numa_node", local}};
    auto candidate = baseline;
    candidate["id"] = "remote";
    candidate["numa_node"] = remote;
    return Json::array({std::move(baseline), std::move(candidate)});
}

auto case_group(std::string_view name, std::string_view task, Json parameters, Json cases) -> Json {
    return {{"schema", scenario::package_scenario_schema},
            {"mode", "cases"},
            {"workloads", {{std::string{name}, {{"task", task}, {"parameters", std::move(parameters)}}}}},
            {"cases", std::move(cases)}};
}

auto build_numa_matrix(Prompts& prompt, const NumaMatrix& matrix, core::CoreId controller)
    -> std::optional<Json> {
    constexpr std::uint64_t kib = 1024U;
    const auto selected = matrix_readers(matrix, controller);
    prompt.out << "Worker NUMA nodes and selected reader CPUs\n";
    for (const auto& [node, cpu] : selected)
        prompt.out << "  NUMA " << node << "  CPU " << cpu.value() << '\n';
    prompt.out << "Memory NUMA nodes";
    for (const auto* node : matrix.targets)
        prompt.out << ' ' << *node->linux_node_id.value();
    prompt.out << '\n';
    const auto size = prompt.number("Working set per reader in KiB (Enter for the default)",
                                    matrix.minimum_kib, matrix.maximum_kib,
                                    std::clamp(kib, matrix.minimum_kib, matrix.maximum_kib));
    if (!size) return {};
    return numa_scenario(matrix, selected, *size);
}

auto build_cache_sweep(Prompts& prompt, const std::vector<CacheSweepSite>& sites) -> std::optional<Json> {
    prompt.out << "Reader CPUs and cache-relative working sets (bytes)\n";
    for (const auto& site : sites) {
        prompt.out << "  CPU " << site.worker->linux_cpu_id.value()
                   << "  local NUMA " << *site.local->linux_node_id.value();
        for (const auto* cache : site.caches)
            prompt.out << "  L" << cache->level << ' ' << cache->size.value() << " B";
        prompt.out << "  sizes";
        for (const auto bytes : site.working_sets) prompt.out << ' ' << bytes;
        prompt.out << '\n';
    }
    const auto site = [&] {
        while (const auto cpu = prompt.number("Reader Linux CPU", sites.front().worker->linux_cpu_id.value(),
                                              sites.back().worker->linux_cpu_id.value())) {
            const auto found = std::ranges::find_if(sites, [cpu](const auto& entry) {
                return entry.worker->linux_cpu_id.value() == *cpu;
            });
            if (found != sites.end()) return found;
            prompt.out << "Choose a reader Linux CPU from the displayed list.\n";
        }
        return sites.end();
    }();
    if (site == sites.end()) return {};
    prompt.out << "Remote NUMA nodes";
    for (const auto* node : site->remote) prompt.out << ' ' << *node->linux_node_id.value();
    prompt.out << '\n';
    const auto remote = [&]() -> std::optional<std::uint32_t> {
        while (const auto node = prompt.number("Remote Linux NUMA node",
                    *site->remote.front()->linux_node_id.value(), *site->remote.back()->linux_node_id.value())) {
            if (std::ranges::any_of(site->remote, [node](const auto* item) {
                    return item->linux_node_id.value() == *node;
                })) return static_cast<std::uint32_t>(*node);
            prompt.out << "Choose a remote Linux NUMA node from the displayed list.\n";
        }
        return {};
    }();
    if (!remote) return {};
    return cache_scenario(*site, *remote);
}

auto author_execution(Prompts& prompt, std::size_t default_repetitions, std::size_t default_blocks,
                      bool streaming, bool fixed_repetitions = false) -> std::optional<Json> {
    const auto repetitions = fixed_repetitions ? std::optional{default_repetitions}
        : prompt.number("Repetitions per callback (Enter for " +
                        std::to_string(default_repetitions) + ")", 1, 1000000000, default_repetitions);
    if (!repetitions) return {};
    prompt.out << "For additional observation controls enter a JSON object with blocks and the "
                  "desired options";
    if (streaming)
        prompt.out << ", for example {\"blocks\":4,\"pipeline_chunks\":2}";
    prompt.out << ".\n";
    for (;;) {
        auto answer = prompt.line("Measured blocks, or execution options (Enter for " +
                                  std::to_string(default_blocks) + ")");
        if (!answer) return {};
        if (answer->empty())
            return Json{{"repetitions", *repetitions}, {"blocks", default_blocks}};
        auto value = Json::parse(*answer, nullptr, false);
        if (value.is_number_unsigned() && value >= 1 && value <= 4096)
            return Json{{"repetitions", *repetitions}, {"blocks", std::move(value)}};
        if (value.is_object() && value.contains("blocks") && !value.contains("repetitions")) {
            if (value.contains("controller_core")) {
                prompt.out << "Choose the physical core at the controller prompt.\n";
                continue;
            }
            value["repetitions"] = *repetitions;
            return value;
        }
        prompt.out << "Enter 1 to 4096 blocks or an object containing blocks and observation controls.\n";
    }
}

auto select_controller(Prompts& prompt, const ControllerChoices& choices) -> std::optional<std::size_t> {
    while (auto answer = prompt.line("Controller physical core (Enter for automatic, m to choose)")) {
        if (answer->empty()) return 0U;
        if (*answer == "m") {
            for (std::size_t i = 0; i < choices.size(); ++i)
                prompt.out << i + 1U << ") " << choices[i].core.value()
                           << "  Linux CPU " << choices[i].cpu.value() << '\n';
            return prompt.number("Controller physical core (Enter for automatic)", 0, choices.size(), 0);
        }
        prompt.out << "Press Enter for automatic placement or enter m to choose a physical core.\n";
    }
    return {};
}

void print_controller(std::ostream& out, const scenario::PackagePlan& plan) {
    if (plan.controller)
        out << "Controller Linux CPU " << plan.controller->cpu.value() << " on physical core "
            << plan.controller->core.value() << '\n';
}

auto write_scenario(Json base, const Catalog& catalog,
                    const scenario::ResourceResolutionContext& context,
                    const std::optional<std::string>& destination,
                    std::ostream& output, std::ostream& diagnostics) -> int {
    if (destination) {
        if (!run_output_directory::valid(*destination)) {
            diagnostics << "Expected an absolute Result directory.\n";
            return 6;
        }
        base["output"] = {{"directory", *destination}, {"format", "json"}};
    }
    if (scenario::is_package_case_group(base)) {
        auto resolved = scenario::resolve_package_cases(base, catalog, context);
        if (!resolved) return report(resolved.error(), diagnostics);
        diagnostics << resolved->size() << " independent comparison cases.\n";
        print_controller(diagnostics, resolved->front().plan);
    } else {
        auto resolved = scenario::resolve_package(base, catalog, context);
        if (!resolved) return report(resolved.error(), diagnostics);
        print_controller(diagnostics, *resolved);
    }
    output << base.dump(2) << '\n';
    return output ? 0 : 1;
}

auto check_recipe(const Json& recipe, const scenario::ResourceResolutionContext& context,
                  std::ostream& diagnostics) -> bool {
    const auto bytes = recipe.dump();
    const auto maximum = recipe.contains("scenario") &&
        scenario::is_package_case_group(recipe.at("scenario")) ? scenario::maximum_package_cases : 64U;
    auto space = scenario::ConfigurationSpace::parse(
        {"sweep", bytes, scenario::ScenarioFormat::json}, maximum);
    if (!space) {
        report(space.error(), diagnostics);
        return false;
    }
    for (std::size_t i = 0; i < space->size(); ++i) {
        auto row = space->resolve(i, context);
        if (!row) {
            report(row.error(), diagnostics);
            return false;
        }
        if (!row->accepted()) {
            for (const auto& note : row->diagnostics)
                diagnostics << "Candidate " << i << ' ' << note.code.value() << ' '
                            << note.json_pointer << ' ' << Json(note.safe_context) << '\n';
            return false;
        }
    }
    return true;
}
} // namespace

namespace scenario_guides {

auto memory_guides(const Catalog& catalog, const scenario::ResourceResolutionContext& context)
    -> MemoryGuides {
    const auto& machine = context.topology.machine();
    auto workers = eligible_workers(context.topology);
    std::erase_if(workers, [&](const auto* worker) {
        return !machine.online_cpus.contains(worker->linux_cpu_id);
    });
    const auto nodes = base_page_nodes(context.topology, eligible_nodes(context.topology));
    MemoryGuides guides;
    if (catalog.find(bandwidth_task, bandwidth_implementation))
        guides.matrix = numa_matrix_sites(context, workers, nodes);
    if (catalog.find(latency_task, latency_implementation))
        guides.cache_sites = cache_sweep_sites(context.topology, workers, nodes);
    return guides;
}

auto guide_controller_choices(const Json& execution, const scenario::ResourceResolutionContext& context,
                              MemoryGuides& guides, bool bandwidth)
    -> ControllerChoices {
    auto choices = scenario::package_controller_choices(
        {{"roles", Json::array()}, {"execution", execution}}, context);
    // Any separate monitoring core is excluded from both controller and reader choices.
    std::set<core::CoreId> available_cores;
    for (const auto& choice : choices)
        available_cores.insert(*context.stable_ids.core(choice.core));
    if (bandwidth) {
        for (auto& entry : guides.matrix.readers)
            std::erase_if(entry.second, [&](const auto* worker) {
                return !available_cores.contains(worker->core_id);
            });
    } else {
        std::erase_if(guides.cache_sites, [&](const auto& site) {
            return !available_cores.contains(site.worker->core_id);
        });
    }
    std::erase_if(choices, [&](const auto& choice) {
        const auto controller = *context.stable_ids.core(choice.core);
        if (bandwidth)
            return std::ranges::any_of(guides.matrix.readers, [controller](const auto& node) {
                return std::ranges::none_of(node.second, [controller](const auto* worker) {
                    return worker->core_id != controller;
                });
            });
        return std::ranges::none_of(guides.cache_sites, [controller](const auto& site) {
            return site.worker->core_id != controller;
        });
    });
    return choices;
}

auto matrix_readers(const NumaMatrix& matrix, core::CoreId controller)
    -> std::map<std::uint32_t, core::LinuxCpuId> {
    std::map<std::uint32_t, core::LinuxCpuId> selected;
    for (const auto& [node, readers] : matrix.readers) {
        const auto reader = std::ranges::find_if(readers, [controller](const auto* worker) {
            return worker->core_id != controller;
        });
        if (reader == readers.end()) return {};
        selected.emplace(node, (*reader)->linux_cpu_id);
    }
    return selected;
}

auto numa_scenario(const NumaMatrix& matrix, const std::map<std::uint32_t, core::LinuxCpuId>& selected,
                   std::uint64_t working_set_kib) -> Json {
    constexpr std::uint64_t kib = 1024U;
    auto cases = Json::array();
    for (const auto& [local, cpu] : selected) {
        for (const auto* target : matrix.targets) {
            const auto remote = *target->linux_node_id.value();
            if (remote == local) continue;
            cases.push_back({{"id", "numa-" + std::to_string(local) + "-to-" + std::to_string(remote)},
                             {"roles", case_roles("read", bandwidth_implementation, cpu.value(), local, remote)}});
        }
    }
    return case_group("read", bandwidth_task, {{"working_set_lines", working_set_kib * (kib / dense_memory_line_bytes)}},
                      std::move(cases));
}

auto cache_scenario(const CacheSweepSite& site, std::uint32_t remote) -> Json {
    const auto local = *site.local->linux_node_id.value();
    const auto roles = case_roles("chain", latency_implementation, site.worker->linux_cpu_id.value(), local, remote);
    auto cases = Json::array();
    for (const auto bytes : site.working_sets)
        cases.push_back({{"id", "latency-" + std::to_string(bytes) + "-bytes"},
                         {"roles", roles},
                         {"parameters", {{"working_set_lines", bytes / dense_memory_line_bytes}}}});
    return case_group("chain", latency_task,
                      {{"working_set_lines", *site.working_sets.begin() / dense_memory_line_bytes},
                       {"chain_seed", 23U}, {"start_seed", 19U}}, std::move(cases));
}

} // namespace scenario_guides

auto author_package_scenario(const Catalog& catalog,
                             const scenario::ResourceResolutionContext& context,
                             const std::optional<std::string>& destination, std::istream& in,
                             std::ostream& output, std::ostream& diagnostics) -> int {
    Prompts prompt{in, diagnostics};
    const std::array<std::string_view, 4> modes{"standalone", "comparison", "mixed", "pipeline"};
    const auto& machine = context.topology.machine();
    if (!machine.effective_cpus.value()) {
        diagnostics << "The supplied topology must identify effective CPUs.\n";
        return 6;
    }
    auto guides = memory_guides(catalog, context);
    auto& [matrix, cache_sites] = guides;
    diagnostics << "1) Standalone\n2) Compare implementations of a task\n3) Mixed workloads\n4) "
                   "Staged pipeline\n"
                   "5) NUMA node-pair read bandwidth matrix";
    if (matrix.readers.empty())
        diagnostics << " (requires an installed read package and eligible local/remote NUMA pairs)";
    diagnostics << "\n6) Dependent-load latency around cache capacities";
    if (cache_sites.empty())
        diagnostics << " (requires an installed latency package and eligible cache and NUMA placements)";
    diagnostics << '\n';
    const auto mode = [&]() -> std::optional<std::size_t> {
        while (const auto selected = prompt.number("Execution mode or guide", 1, 6)) {
            if ((*selected == 5U && matrix.readers.empty()) ||
                (*selected == 6U && cache_sites.empty())) {
                diagnostics << "This guide is unavailable with the saved topology and installed packages.\n";
                continue;
            }
            return selected;
        }
        return {};
    }();
    if (!mode)
        return 0;
    if (*mode > modes.size()) {
        const bool bandwidth = *mode == 5U;
        auto execution = author_execution(prompt, bandwidth ? 8U : 8192U, 4U, false);
        if (!execution) return 0;
        auto choices = guide_controller_choices(*execution, context, guides, bandwidth);
        if (choices.empty()) {
            diagnostics << (bandwidth
                ? "A separate controller core must leave a reader on every eligible reader NUMA node.\n"
                : "A separate controller core must leave an eligible cache reader.\n");
            return 6;
        }
        const auto selected = select_controller(prompt, choices);
        if (!selected) return 0;
        const auto& controller = choices[*selected == 0U ? 0U : *selected - 1U];
        const auto controller_core = *context.stable_ids.core(controller.core);
        std::erase_if(cache_sites, [controller_core](const auto& site) {
            return site.worker->core_id == controller_core;
        });
        auto base = bandwidth ? build_numa_matrix(prompt, matrix, controller_core)
                              : build_cache_sweep(prompt, cache_sites);
        if (!base) return 0;
        (*execution)["controller_core"] = controller.core.value();
        (*base)["execution"] = std::move(*execution);
        auto result_directory = destination;
        if (!result_directory) {
            for (;;) {
                auto path = prompt.line("Absolute private Result directory (Enter to omit)");
                if (!path) return 0;
                if (path->empty()) break;
                if (run_output_directory::valid(*path)) {
                    result_directory = std::move(path);
                    break;
                }
                diagnostics << "Expected an absolute Result directory.\n";
            }
        }
        return write_scenario(std::move(*base), catalog, context, result_directory, output, diagnostics);
    }
    const auto roles = *mode < 3 ? mode : prompt.number("Number of roles or stages", 2, 64);
    if (!roles)
        return 0;
    Json base{{"schema", scenario::package_scenario_schema},
              {"mode", modes[*mode - 1]},
              {"workloads", Json::object()},
              {"roles", Json::array()}};
    std::string shared_task;
    const ImplementationManifest* predecessor{};
    for (std::size_t i = 0; i < *roles; ++i) {
        const std::string role_id =
            *mode == 2 ? (i == 0 ? "baseline" : "candidate") : "role-" + std::to_string(i + 1);
        diagnostics << "Installed implementations for " << role_id << '\n';
        std::vector<const ImplementationManifest*> choices;
        for (const auto& impl : catalog.implementations()) {
            if (*mode == 2 && i != 0 && impl.task->id != shared_task)
                continue;
            if (*mode != 1 && impl.task->mapping != MappingResource::none)
                continue;
            choices.push_back(&impl);
            diagnostics << choices.size() << ") " << impl.task->id << "  " << impl.id << '\n';
        }
        if (choices.empty()) {
            diagnostics << "No compatible packages installed.\n";
            return 6;
        }
        const auto selected = prompt.number("Implementation", 1, choices.size());
        if (!selected)
            return 0;
        const auto& implementation = *choices[*selected - 1];
        const std::string workload_id = *mode == 2 ? "shared" : role_id;
        if (!base["workloads"].contains(workload_id)) {
            shared_task = implementation.task->id;
            diagnostics << "Task inputs for " << shared_task << '\n';
            const auto inputs = prompt.settings(implementation.task->parameters);
            if (!inputs)
                return 0;
            base["workloads"][workload_id] = {{"task", shared_task}, {"parameters", *inputs}};
        }
        if (!implementation.tuning.empty())
            diagnostics << "Tuning for " << implementation.id << '\n';
        const auto tuning = prompt.settings(implementation.tuning);
        if (!tuning)
            return 0;
        diagnostics << "Effective Linux CPUs " << machine.effective_cpus.value()->to_range_list() << '\n';
        const auto cpu = prompt.indices("Worker Linux CPU or ordered JSON CPU list");
        if (!cpu)
            return 0;
        base["roles"].push_back({{"id", role_id},
                                 {"workload", workload_id},
                                 {"implementation", implementation.id},
                                 {"tuning", *tuning},
                                 {"cpus", cpu->is_array() ? *cpu : Json::array({*cpu})}});
        const auto resource = implementation.task->mapping;
        if (resource != MappingResource::none) {
            const auto service = prompt.number("Service Linux CPU on a different physical core", 0, UINT32_MAX);
            if (!service)
                return 0;
            base["roles"].back()["service_cpu"] = *service;
        }
        if (resource != MappingResource::allocation) {
            if (!machine.effective_nodes.value()) {
                diagnostics << "The supplied topology must identify effective memory nodes.\n";
                return 6;
            }
            diagnostics << "Effective NUMA nodes " << machine.effective_nodes.value()->to_range_list() << '\n';
            if (resource == MappingResource::first_touch) {
                const auto node = prompt.number("Worker's local Linux NUMA node", 0, UINT32_MAX);
                if (!node) return 0;
                base["roles"].back()["numa_node"] = *node;
            } else {
                diagnostics << "For input policies, enter a placement object such as "
                               "{\"numa_node\":0,\"input_placement\":{\"policy\":\"interleave\",\"nodes\":["
                               "0,1]}}.\n";
                const auto node = prompt.indices("Memory NUMA node, per-worker JSON node list or placement object", true);
                if (!node) return 0;
                if (node->is_object())
                    base["roles"].back().update(*node);
                else
                    base["roles"].back()[node->is_array() ? "numa_nodes" : "numa_node"] = *node;
            }
        }
        if (*mode == 4 && predecessor) {
            auto links = Json::object();
            for (const auto& [name, format] : implementation.task->ports.inputs) {
                std::vector<std::string> options;
                diagnostics << "Connect input " << name << " (" << format << ")\n";
                for (const auto& [port, type] : predecessor->task->ports.outputs) {
                    if (type != format)
                        continue;
                    options.push_back(port);
                    diagnostics << options.size() << ") " << port << '\n';
                }
                if (options.empty())
                    continue;
                const auto selected_port = prompt.number(
                    "Output of previous stage (0 to use generated input)", 0, options.size());
                if (!selected_port)
                    return 0;
                if (*selected_port)
                    links[name] = options.at(*selected_port - 1U);
            }
            if (links.empty()) {
                diagnostics << "These stages need at least one compatible port connection.\n";
                return 6;
            }
            base["roles"].back()["inputs"] = std::move(links);
        }
        predecessor = &implementation;
    }
    auto execution = author_execution(prompt, 1U, 1U, *mode == 4U,
                                      predecessor->task->mapping != MappingResource::none);
    if (!execution) return 0;
    base["execution"] = std::move(*execution);
    const auto choices = scenario::package_controller_choices(base, context);
    if (!choices.empty()) {
        const auto selected = select_controller(prompt, choices);
        if (!selected) return 0;
        if (*selected)
            base["execution"]["controller_core"] = choices[*selected - 1U].core.value();
    }
    return write_scenario(std::move(base), catalog, context, destination, output, diagnostics);
}

auto author_package_sweep(const Json& document, bool edit,
                          const scenario::ResourceResolutionContext& context, std::istream& in,
                          std::ostream& out, std::ostream& diagnostics) -> int {
    Prompts prompt{in, diagnostics};
    if (edit && !check_recipe(document, context, diagnostics))
        return 6;
    const auto& base = edit ? document.at("scenario") : document;
    const bool case_group = scenario::is_package_case_group(base);
    Json effective;
    if (edit) {
        const auto source = case_group ? scenario::materialize_package_case(base, 0) : base;
        auto settings = scenario::validate_package_sweep_base(source, *context.packages, context);
        if (!settings) return report(settings.error(), diagnostics);
        effective = std::move(*settings);
    } else if (case_group) {
        auto cases = scenario::resolve_package_cases(base, *context.packages, context);
        if (!cases) return report(cases.error(), diagnostics);
        effective = std::move(cases->front().plan.evidence.at("scenario"));
    } else {
        auto resolved = scenario::resolve_package(base, *context.packages, context);
        if (!resolved) return report(resolved.error(), diagnostics);
        effective = std::move(resolved->evidence.at("scenario"));
    }
    if (case_group) {
        diagnostics << "Comparison cases\n";
        for (const auto& one : base.at("cases"))
            diagnostics << "  " << one.at("id").get_ref<const std::string&>() << '\n';
    }
    auto axes = scenario::package_axes(base, *context.packages);
    if (!axes)
        return report(axes.error(), diagnostics);
    diagnostics << "Execution mode " << base.at("mode") << '\n';
    for (const auto& role : effective.at("roles")) {
        const auto workload = role.at("workload").get<std::string>();
        diagnostics << role.at("id") << " uses " << role.at("implementation") << " for "
                    << base.at("workloads").at(workload).at("task") << " (" << workload << ")\n";
        diagnostics << "Available implementations";
        for (const auto& implementation : context.packages->implementations())
            if (implementation.task->id ==
                base.at("workloads").at(workload).at("task").get_ref<const std::string&>())
                diagnostics << ' ' << implementation.id;
        diagnostics << '\n';
    }
    Json recipe;
    if (edit)
        recipe = document;
    else {
        diagnostics << "1) Maximize tasks per second\n2) Minimize seconds per completed task\n";
        const auto objective = prompt.number("Objective", 1, 2);
        if (!objective)
            return 0;
        recipe = {
            {"schema", scenario::package_space_schema},
            {"objective", *objective == 1 ? "tasks_per_second" : "seconds_per_completed_task"},
            {"scenario", base},
            {"axes", Json::array()}};
    }
    for (std::size_t i = 0; i < axes->size(); ++i) {
        const auto& path = axes->at(i);
        diagnostics << i + 1 << ") " << path << " base "
                    << effective.at(Json::json_pointer(path)) << '\n';
    }
    for (;;) {
        const auto selected =
            prompt.number("Axis to add or replace (Enter to finish)", 0, axes->size(), 0);
        if (!selected)
            return 0;
        if (*selected == 0)
            break;
        auto answer = prompt.line("Choices as a JSON array");
        if (!answer)
            return 0;
        Json values = Json::parse(*answer, nullptr, false);
        if (!values.is_array() || values.empty()) {
            diagnostics << "Enter a nonempty JSON array.\n";
            continue;
        }
        auto trial = recipe;
        auto& definitions = trial["axes"];
        const auto& path = axes->at(*selected - 1);
        auto it = std::ranges::find_if(definitions,
                                       [&](const auto& axis) { return axis.at("path") == path; });
        Json axis{{"path", path}, {"values", std::move(values)}};
        if (it == definitions.end())
            definitions.push_back(std::move(axis));
        else
            *it = std::move(axis);
        if (check_recipe(trial, context, diagnostics))
            recipe = std::move(trial);
    }
    if (!check_recipe(recipe, context, diagnostics))
        return 6;
    out << recipe.dump(2) << '\n';
    return out ? 0 : 1;
}
} // namespace epycbench::cli
