#include "epycbench/monitoring/host_environment.hpp"
#include "core/json_ownership.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "monitoring/host_environment_access.hpp"
#include "monitoring/host_qualification_plan_access.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
#include "monitoring/privacy_validation.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace epycbench::monitoring::qualification {

class detail_HostEnvironmentFactory {
  public:
    static auto make(std::vector<HostEnvironmentEligibleNode> nodes,
                     std::vector<HostEnvironmentWorker> bandwidth,
                     std::vector<HostEnvironmentWorker> latency,
                     std::vector<HostEnvironmentServiceCore> service, core::ByteCount page_size,
                     topology::PhysicalTopologyFingerprint physical,
                     topology::EffectiveTopologyFingerprint effective,
                     std::shared_ptr<const detail_HostSessionBinding> binding,
                     std::string canonical, core::Sha256Digest digest) -> HostEnvironmentAuthority {
        return {std::move(nodes),
                std::move(bandwidth),
                std::move(latency),
                std::move(service),
                page_size,
                std::move(physical),
                std::move(effective),
                std::move(binding),
                std::move(canonical),
                digest};
    }
};

namespace {
using Json = nlohmann::json;
struct EligibleCore {
    core::CoreId handle;
    topology::StableCoreId stable;
    core::NumaNodeId node;
    std::vector<std::pair<topology::StableProcessingUnitId, core::LinuxCpuId>> members;
};
struct EligibleNode {
    core::NumaNodeId handle;
    topology::StableNumaNodeId stable;
    core::ByteCount installed_memory;
    std::vector<EligibleCore> cores;
};

auto failure(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.host_environment.derivation_rejected"},
            .message = "Host environment derivation failed",
            .operation = "derive_host_environment",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& json_value_1) {
                core::detail::set_json(json_value_1, "classification", std::move(classification));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto contained(bool unknown = false) -> core::Error {
    auto result = failure(unknown ? "contained_unknown_failure" : "contained_exception");
    core::detail::JsonCleanup result_cleanup{result.context};
    result.code = core::StableErrorCode{"monitoring.host_environment.contained_failure"};
    return core::Error(std::move(result));
}
auto available_string(const topology::Attribute<std::string>& value) -> std::optional<std::string> {
    if (value.availability() != topology::Availability::available || !value.value())
        return std::nullopt;
    return *value.value();
}
auto normalize_model(std::string_view input) -> std::string {
    std::string output;
    bool pending_space = false;
    for (const char character : input) {
        if (character == ' ') {
            pending_space = !output.empty();
            continue;
        }
        if (pending_space)
            output.push_back(' ');
        pending_space = false;
        output.push_back(character);
    }
    return output;
}
auto ascii_alnum(char value) -> bool {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) ||
           (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
           (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9'));
}
auto has_epyc_token(std::string_view value) -> bool {
    std::size_t position = value.find("EPYC");
    while (position != std::string_view::npos) {
        const bool left = position == 0 || !ascii_alnum(value[position - 1]);
        const auto end = position + 4U;
        const bool right = end == value.size() || !ascii_alnum(value[end]);
        if (left && right)
            return true;
        position = value.find("EPYC", position + 1U);
    }
    return false;
}
template <class Attribute> auto available_set(const Attribute& value) -> bool {
    return value.availability() == topology::Availability::available && value.value().has_value();
}

auto project_worker(const HostEnvironmentWorker& worker) -> Json {
    return core::detail::json_object<Json>([&](auto& json_value_10001) {
        core::detail::set_json(json_value_10001, "linux_cpu", Json(worker.linux_cpu.value()));
        core::detail::set_json(json_value_10001, "stable_core_id",
                               Json(worker.stable_core_id.value()));
        core::detail::set_json(json_value_10001, "stable_numa_node_id",
                               Json(worker.stable_numa_node_id.value()));
        core::detail::set_json(json_value_10001, "stable_pu_id", Json(worker.stable_pu_id.value()));
        core::detail::set_json(json_value_10001, "worker_id", Json(worker.worker_id));
    });
}
auto project_service(const HostEnvironmentServiceCore& service) -> Json {
    Json pus = Json::array();
    core::detail::JsonCleanup pus_cleanup{pus};
    for (const auto& pu : service.stable_pu_ids)
        pus.push_back(pu.value());
    Json cpus = Json::array();
    core::detail::JsonCleanup cpus_cleanup{cpus};
    for (const auto cpu : service.linux_cpus)
        cpus.push_back(cpu.value());
    return core::detail::json_object<Json>([&](auto& json_value_10002) {
        core::detail::set_json(json_value_10002, "execution_linux_cpu",
                               Json(service.execution_linux_cpu.value()));
        core::detail::set_json(json_value_10002, "execution_stable_pu_id",
                               Json(service.execution_stable_pu_id.value()));
        core::detail::set_json(json_value_10002, "linux_cpus", std::move(cpus));
        core::detail::set_json(json_value_10002, "role", Json(service.role));
        core::detail::set_json(json_value_10002, "stable_core_id",
                               Json(service.stable_core_id.value()));
        core::detail::set_json(json_value_10002, "stable_pu_ids", std::move(pus));
    });
}
} // namespace

auto detail::checked_host_worker_id(std::size_t index) noexcept
    -> std::expected<std::uint32_t, core::Error> {
    return checked_host_worker_id_fallible(index);
}
auto detail::checked_host_worker_id_fallible(std::size_t index)
    -> std::expected<std::uint32_t, core::Error> {
    if (index > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected{failure("worker_cardinality_exceeded")};
    return static_cast<std::uint32_t>(index);
}

auto derive_host_environment(const RunnerContextAcquisitionSession& session) noexcept
    -> std::expected<HostEnvironmentAuthority, core::Error> {
    try {
        return detail::derive_host_environment_fallible(session);
    } catch (const std::exception&) {
        return std::unexpected{contained()};
    } catch (...) {
        return std::unexpected{contained(true)};
    }
}
auto detail::derive_host_environment_fallible(const RunnerContextAcquisitionSession& session,
                                              std::optional<core::Error>* raw_failure)
    -> std::expected<HostEnvironmentAuthority, core::Error> {
    auto context_view = detail_RunnerContextAcquisitionAccess::runner_context_fallible(session);
    core::detail::ExpectedErrorCleanup context_view_cleanup{context_view};
    if (!context_view) {
        if (raw_failure && !*raw_failure)
            raw_failure->emplace(std::move(context_view.error()));
        return std::unexpected{failure("invalid_session")};
    }
    auto prerequisite_view = detail_RunnerContextAcquisitionAccess::prerequisites_fallible(session);
    core::detail::ExpectedErrorCleanup prerequisite_view_cleanup{prerequisite_view};
    if (!prerequisite_view) {
        if (raw_failure && !*raw_failure)
            raw_failure->emplace(std::move(prerequisite_view.error()));
        return std::unexpected{failure("invalid_session")};
    }
    auto binding = detail_HostQualificationPlanAccess::binding_fallible(session);
    core::detail::ExpectedErrorCleanup binding_cleanup{binding};
    if (!binding) {
        if (raw_failure && !*raw_failure)
            raw_failure->emplace(std::move(binding.error()));
        return std::unexpected{failure("invalid_session")};
    }
    const auto& context = context_view->get();
    const auto& prerequisites = prerequisite_view->get();
    if (!prerequisites.linux())
        return std::unexpected{failure("linux_platform_required")};
    const auto& topology = context.topology();
    const auto& machine = topology.machine();
    if (machine.architecture != "x86_64")
        return std::unexpected{failure("architecture_unsupported")};
    const auto vendor = available_string(machine.cpu_vendor);
    if (!vendor || *vendor != "AuthenticAMD")
        return std::unexpected{failure("cpu_vendor_unavailable_or_unsupported")};
    const auto model_value = available_string(machine.cpu_model);
    if (!model_value)
        return std::unexpected{failure("cpu_model_unavailable")};
    const auto model = normalize_model(*model_value);
    if (!has_epyc_token(model))
        return std::unexpected{failure("cpu_model_not_epyc")};
    if (!available_set(machine.allowed_cpus) || !available_set(machine.effective_cpus) ||
        !available_set(machine.allowed_nodes) || !available_set(machine.effective_nodes))
        return std::unexpected{failure("effective_topology_unavailable")};

    std::optional<core::ByteCount> base_page;
    for (const auto size : machine.page_sizes)
        if (size.value() > 0U && (!base_page || size < *base_page))
            base_page = size;
    if (!base_page)
        return std::unexpected{failure("base_page_size_unavailable")};

    std::map<core::ProcessingUnitId, const topology::ProcessingUnit*> pus;
    for (const auto& pu : topology.processing_units())
        pus.emplace(pu.id, &pu);
    std::vector<EligibleNode> nodes;
    for (const auto& node : topology.numa_nodes()) {
        if (node.linux_node_id.availability() != topology::Availability::available ||
            !node.linux_node_id.value() ||
            !machine.effective_nodes.value()->contains(
                core::NumaNodeId{*node.linux_node_id.value()}) ||
            node.installed_memory.availability() != topology::Availability::available ||
            !node.installed_memory.value() || node.installed_memory.value()->value() == 0U ||
            std::ranges::find(node.page_sizes, *base_page) == node.page_sizes.end())
            continue;
        auto stable_node = context.stable_ids().numa_node_id(node.id);
        if (!stable_node)
            return std::unexpected{failure("stable_id_unavailable")};
        EligibleNode eligible{node.id, stable_node->get(), *node.installed_memory.value(), {}};
        for (const auto& core : topology.cores()) {
            const auto members = topology.core_members(core.id);
            if (members.empty())
                continue;
            auto stable_core = context.stable_ids().core_id(core.id);
            if (!stable_core)
                return std::unexpected{failure("stable_id_unavailable")};
            EligibleCore candidate{core.id, stable_core->get(), node.id, {}};
            bool accepted = true;
            for (const auto pu_handle : members) {
                const auto found = pus.find(pu_handle);
                if (found == pus.end()) {
                    accepted = false;
                    break;
                }
                const auto& pu = *found->second;
                const auto memberships = topology.numa_nodes_for_pu(pu.id);
                auto stable_pu = context.stable_ids().processing_unit_id(pu.id);
                if (!stable_pu || memberships.size() != 1U || memberships.front() != node.id ||
                    !machine.online_cpus.contains(pu.linux_cpu_id) ||
                    !machine.allowed_cpus.value()->contains(pu.linux_cpu_id) ||
                    !machine.effective_cpus.value()->contains(pu.linux_cpu_id)) {
                    accepted = false;
                    break;
                }
                candidate.members.emplace_back(stable_pu->get(), pu.linux_cpu_id);
            }
            if (accepted) {
                std::ranges::sort(candidate.members, {},
                                  [](const auto& member) { return member.first; });
                eligible.cores.push_back(std::move(candidate));
            }
        }
        std::ranges::sort(eligible.cores, {}, &EligibleCore::stable);
        if (!eligible.cores.empty())
            nodes.push_back(std::move(eligible));
    }
    std::ranges::sort(nodes, {}, &EligibleNode::stable);
    if (nodes.empty())
        return std::unexpected{failure("eligible_numa_empty")};

    std::vector<HostEnvironmentWorker> bandwidth;
    std::set<core::CoreId> worker_cores;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        auto worker_id = detail::checked_host_worker_id_fallible(index);
        if (!worker_id)
            return std::unexpected{std::move(worker_id.error())};
        const auto& core = nodes[index].cores.front();
        const auto& pu = core.members.front();
        bandwidth.push_back({*worker_id, nodes[index].stable, core.stable, pu.first, pu.second});
        worker_cores.insert(core.handle);
    }
    std::vector<HostEnvironmentWorker> latency{bandwidth.front()};
    latency.front().worker_id = 0;

    std::vector<const EligibleCore*> remaining;
    for (const auto& node : nodes)
        for (const auto& core : node.cores)
            if (!worker_cores.contains(core.handle))
                remaining.push_back(&core);
    std::ranges::sort(remaining, {}, [](const EligibleCore* core) { return core->stable; });
    if (remaining.size() < 2U)
        return std::unexpected{failure("two_service_cores_required")};
    std::vector<HostEnvironmentServiceCore> service;
    for (std::size_t index = 0; index < 2U; ++index) {
        const auto& core = *remaining[index];
        std::vector<topology::StableProcessingUnitId> stable_pus;
        std::vector<core::LinuxCpuId> linux_cpus;
        for (const auto& [stable, cpu] : core.members) {
            stable_pus.push_back(stable);
            linux_cpus.push_back(cpu);
        }
        service.push_back({index == 0U ? "control" : "measurement", core.stable,
                           std::move(stable_pus), std::move(linux_cpus), core.members.front().first,
                           core.members.front().second});
    }

    Json eligible_json = Json::array();
    core::detail::JsonCleanup eligible_json_cleanup{eligible_json};
    std::vector<HostEnvironmentEligibleNode> eligible_nodes;
    for (const auto& node : nodes) {
        eligible_nodes.push_back({node.stable, node.installed_memory});
        core::detail::append_json(
            eligible_json, core::detail::json_object<Json>([&](auto& json_value_11) {
                core::detail::set_json(json_value_11, "installed_memory_bytes",
                                       Json(node.installed_memory.value()));
                core::detail::set_json(json_value_11, "stable_numa_node_id",
                                       Json(node.stable.value()));
            }));
    }
    Json bandwidth_json = Json::array();
    core::detail::JsonCleanup bandwidth_json_cleanup{bandwidth_json};
    for (const auto& worker : bandwidth)
        core::detail::append_json(bandwidth_json, project_worker(worker));
    Json latency_json = Json::array();
    core::detail::JsonCleanup latency_json_cleanup{latency_json};
    for (const auto& worker : latency)
        core::detail::append_json(latency_json, project_worker(worker));
    Json service_json = Json::array();
    core::detail::JsonCleanup service_json_cleanup{service_json};
    for (const auto& item : service)
        core::detail::append_json(service_json, project_service(item));
    Json document = core::detail::json_object<Json>([&](auto& json_value_2) {
        core::detail::set_json(json_value_2, "bandwidth_workers", std::move(bandwidth_json));
        core::detail::set_json(
            json_value_2, "capabilities", core::detail::json_object<Json>([&](auto& json_value_3) {
                core::detail::set_json(json_value_3, "authentic_amd", Json(true));
                core::detail::set_json(json_value_3, "bandwidth_kernel", Json(true));
                core::detail::set_json(json_value_3, "base_page_size", Json(true));
                core::detail::set_json(json_value_3, "calibration", Json(true));
                core::detail::set_json(json_value_3, "eligible_numa_nonempty", Json(true));
                core::detail::set_json(json_value_3, "epyc_model", Json(true));
                core::detail::set_json(json_value_3, "fingerprints", Json(true));
                core::detail::set_json(json_value_3, "latency_kernel", Json(true));
                core::detail::set_json(json_value_3, "linux", Json(true));
                core::detail::set_json(json_value_3, "stable_ids", Json(true));
                core::detail::set_json(json_value_3, "topology_valid", Json(true));
                core::detail::set_json(json_value_3, "two_service_cores", Json(true));
                core::detail::set_json(json_value_3, "x86_64", Json(true));
            }));
        core::detail::set_json(json_value_2, "latency_workers", std::move(latency_json));
        core::detail::set_json(
            json_value_2, "platform", core::detail::json_object<Json>([&](auto& json_value_4) {
                core::detail::set_json(json_value_4, "architecture", Json(machine.architecture));
                core::detail::set_json(json_value_4, "cpu_model", Json(model));
                core::detail::set_json(json_value_4, "cpu_vendor", Json(*vendor));
                core::detail::set_json(json_value_4, "os", Json("Linux"));
            }));
        core::detail::set_json(json_value_2, "policy_identity",
                               Json("epycbench.monitoring-qualification.host-generic/v2"));
        core::detail::set_json(json_value_2, "schema",
                               Json("epycbench.monitoring-host-environment/v2"));
        core::detail::set_json(json_value_2, "selection_policy",
                               core::detail::json_object<Json>([&](auto& json_value_5) {
                                   core::detail::set_json(
                                       json_value_5, "id",
                                       Json("epycbench.monitoring-host-selection"));
                                   core::detail::set_json(json_value_5, "version", Json(1));
                               }));
        core::detail::set_json(json_value_2, "service_reservation",
                               core::detail::json_object<Json>([&](auto& json_value_6) {
                                   core::detail::set_json(json_value_6, "cores",
                                                          std::move(service_json));
                               }));
        core::detail::set_json(
            json_value_2, "topology", core::detail::json_object<Json>([&](auto& json_value_7) {
                core::detail::set_json(json_value_7, "base_page_size_bytes",
                                       Json(base_page->value()));
                core::detail::set_json(json_value_7, "effective_fingerprint",
                                       Json(context.fingerprints().effective.to_string()));
                core::detail::set_json(json_value_7, "eligible_nodes", std::move(eligible_json));
                core::detail::set_json(json_value_7, "physical_fingerprint",
                                       Json(context.fingerprints().physical.to_string()));
            }));
    });
    core::detail::JsonCleanup document_cleanup{document};
    if (!detail::privacy_safe(document))
        return std::unexpected{failure("privacy_rejected")};
    document["privacy_validation"] = "passed";
    auto canonical = core::canonicalize_json(document);
    core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
    if (!canonical)
        return std::unexpected{std::move(canonical.error())};
    canonical->push_back('\n');
    const auto digest = core::sha256(*canonical);
    auto bound = detail_HostQualificationPlanAccess::bind_environment_fallible(
        session, "sha256:" + digest.to_hex());
    core::detail::ExpectedErrorCleanup bound_cleanup{bound};
    if (!bound)
        return std::unexpected{std::move(bound.error())};
    return detail_HostEnvironmentFactory::make(
        std::move(eligible_nodes), std::move(bandwidth), std::move(latency), std::move(service),
        *base_page, context.fingerprints().physical, context.fingerprints().effective,
        std::move(*binding), std::move(*canonical), digest);
}

auto HostEnvironmentAuthority::environment_digest() const -> std::string {
    return "sha256:" + digest_.to_hex();
}

} // namespace epycbench::monitoring::qualification
