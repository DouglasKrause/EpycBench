#include "runtime/package_admission.hpp"
#include "kernel/package_input_policy.hpp"
#include "kernel/package_mapping.hpp"
#include "platform_linux/package_process.hpp"
#include <set>
#include <stdexcept>

namespace epycbench::runtime {
using Json = nlohmann::json;
using U64 = std::uint64_t;
namespace worker = platform_linux::package_execution;

auto package_input_policy(std::string_view name) -> worker::InputPolicy {
    if (const auto policy = kernel::packages::parse_input_policy(name)) return *policy;
    throw std::invalid_argument("Unknown package input placement policy: " + std::string{name});
}

auto package_input_policy_name(worker::InputPolicy policy) -> std::string_view {
    const auto name = kernel::packages::input_policy_name(policy);
    if (!name.empty()) return name;
    throw std::logic_error("Invalid resolved package input placement policy");
}

auto package_manifest_paths(const Json& plan, const kernel::packages::Catalog& catalog)
    -> std::vector<std::filesystem::path> {
    std::set<std::filesystem::path> paths;
    const auto& scenario = plan.at("scenario");
    for (const auto& role : scenario.at("roles")) {
        const auto& workload = scenario.at("workloads").at(role.at("workload").get<std::string>());
        if (const auto* impl = catalog.find(workload.at("task").get<std::string>(),
                                            role.at("implementation").get<std::string>()))
            paths.insert(impl->path);
    }
    return {paths.begin(), paths.end()};
}

auto package_roles(const Json& plan, const kernel::packages::Catalog& catalog)
    -> std::expected<std::vector<worker::Role>, core::Error> {
    const auto& scenario = plan.at("scenario");
    std::vector<worker::Role> roles;
    for (const auto& role : scenario.at("roles")) {
        const auto& workload = scenario.at("workloads").at(role.at("workload").get<std::string>());
        const auto* impl = catalog.find(workload.at("task").get<std::string>(),
                                        role.at("implementation").get<std::string>());
        if (!impl)
            return std::unexpected{core::Error{core::ErrorCategory::kernel,
                                               core::StableErrorCode{"kernel.package.unknown"},
                                               "Missing package implementation",
                                               "package_roles",
                                               {},
                                               {{"role", role.at("id")}},
                                               core::ErrorSeverity::error,
                                               core::ContinuationPolicy::abort_command}};
        const auto& binding = plan.at("bindings").at(role.at("id").get<std::string>());
        if (binding.at("task_manifest_sha256") != impl->task->sha256.to_hex() ||
            binding.at("oracle_sha256") != impl->task->oracle_sha256.to_hex() ||
            binding.at("implementation_manifest_sha256") != impl->sha256.to_hex() ||
            binding.at("module_sha256") != impl->module_sha256.to_hex())
            return std::unexpected{
                core::Error{core::ErrorCategory::kernel,
                            core::StableErrorCode{"kernel.package.binding"},
                            "Installed task or implementation changed during admission",
                            "package_roles",
                            {},
                            {{"role", role.at("id")}},
                            core::ErrorSeverity::error,
                            core::ContinuationPolicy::abort_command}};
        auto module = worker::Module::load(*impl);
        if (!module)
            return std::unexpected{module.error()};
        worker::Request request;
        request.parameters = workload.at("parameters");
        request.tuning = role.at("tuning");
        request.hardware_counters = scenario.at("execution").value("hardware_counters", false);
        request.repetitions = scenario.at("execution").at("repetitions").get<U64>();
        const auto& place = plan.at("placement").at(roles.size());
        for (const auto& rank : place.at("locality")) {
            const auto value = [&](const char* name) {
                return rank.at(name).is_null() ? UINT32_MAX : rank.at(name).get<std::uint32_t>();
            };
            request.locality.push_back({value("cpu"), value("core"), value("linux_numa_node"),
                                        value("memory_node"), value("package"), value("llc")});
            worker::InspectionContext inspection;
            inspection.workload = role.at("workload").get<std::string>();
            if (!rank.at("worker_id").is_null())
                inspection.worker = rank.at("worker_id").get<std::string>();
            inspection.base_page_bytes = rank.at("base_page_bytes").get<U64>();
            if (const auto& cache = rank.at("l1"); !cache.is_null()) {
                inspection.l1_cache = cache.at("id").get<std::string>();
                inspection.l1_capacity_bytes = cache.at("capacity_bytes").get<U64>();
                inspection.l1_line_bytes = cache.at("line_bytes").get<U64>();
                inspection.l1_ways = cache.at("ways").get<U64>();
            }
            request.inspection.push_back(std::move(inspection));
        }
        std::vector<core::LinuxCpuId> cpus;
        for (const auto& cpu : role.at("cpus"))
            cpus.emplace_back(cpu.get<std::uint32_t>());
        std::vector<worker::StageLink> links;
        if (role.contains("inputs"))
            for (const auto& [input, source] : role.at("inputs").items())
                links.push_back({source.get<std::string>(), input});
        worker::InputPlacement input;
        if (role.contains("input_placement")) {
            const auto& authored = role.at("input_placement");
            input.policy = package_input_policy(authored.at("policy").get<std::string>());
            if (authored.contains("nodes"))
                for (const auto& node : authored.at("nodes"))
                    input.targets.emplace_back(node.get<std::uint32_t>());
            input.replica_group = authored.value("replica_group", std::string{});
        }
        const auto& node = place.at("numa_nodes").at(0);
        roles.push_back({role.at("id").get<std::string>(), std::move(*module), std::move(request),
                         std::move(cpus), core::LinuxNumaNodeId{node.is_null() ? UINT32_MAX : node.get<std::uint32_t>()},
                         std::move(links), std::move(input)});
    }
    return roles;
}

auto inspect_package_layouts(const kernel::packages::Catalog& catalog, const Json& plan,
                             std::uint64_t page_bytes) -> std::expected<Json, core::Error> {
    auto response = platform_linux::package_process::collect(
        {{"operation", "inspect"},
         {"plan", plan},
         {"page_bytes", page_bytes},
         {"manifests", package_manifest_paths(plan, catalog)}});
    if (!response)
        return std::unexpected{response.error()};
    return response->at("geometry");
}

auto inspect_in_runner(const Json& request) -> std::expected<Json, core::Error> {
    const auto files = request.at("manifests").get<std::vector<std::filesystem::path>>();
    auto catalog = kernel::packages::Catalog::load_manifests(files);
    if (!catalog)
        return std::unexpected{catalog.error()};
    if (request.at("operation") == "validate") {
        auto checked = Json::array();
        for (const auto& impl : catalog->implementations()) {
            auto module = worker::Module::load(impl);
            if (!module)
                return std::unexpected{module.error()};
            checked.push_back(kernel::packages::package_identity(impl));
        }
        return Json{{"validated", std::move(checked)}};
    }
    const auto& plan = request.at("plan");
    auto roles = package_roles(plan, *catalog);
    if (!roles)
        return std::unexpected{roles.error()};
    if (plan.contains("mapping") && roles->size() == 1U && roles->front().module.mapping().task)
        return Json{{"geometry", kernel::packages::mapping_geometry(plan.at("scenario").at("roles").at(0))}};
    const auto& mode = plan.at("scenario").at("mode");
    const auto composition = mode == "pipeline" ? worker::Composition::pipeline
                             : mode == "mixed"  ? worker::Composition::mixed
                                                : worker::Composition::sequential;
    const auto page = request.at("page_bytes").get<U64>();
    const auto& execution = plan.at("scenario").at("execution");
    std::optional<std::size_t> short_partition_stage;
    auto geometry = execution.contains("pipeline_chunks")
                        ? worker::inspect_stream_layouts(
                              *roles, execution.at("pipeline_chunks").get<std::size_t>(), page,
                              &short_partition_stage)
                        : worker::inspect_layouts(*roles, composition, page);
    if (!geometry) {
        auto error = std::move(geometry.error());
        if (short_partition_stage) {
            auto paths = Json::array({"/execution/pipeline_chunks"});
            const auto& role = plan.at("scenario").at("roles").at(*short_partition_stage);
            const auto workload_path = Json::json_pointer{} / "workloads" /
                role.at("workload").get_ref<const std::string&>() / "parameters";
            for (const auto& name : roles->at(*short_partition_stage).module.partition_extent_parameters())
                paths.push_back((workload_path / name).to_string());
            error.context[kernel::packages::layout_related_settings] = std::move(paths);
        }
        return std::unexpected{std::move(error)};
    }
    return Json{{"geometry", std::move(*geometry)}};
}
} // namespace epycbench::runtime
