#include "core/json_ownership.hpp"
#include "epycbench/runtime/admitted_placement_request.hpp"
#include "pairwise_v2_contextual_admission_classification.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/invariants.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace epycbench::runtime {
namespace {
using Json = nlohmann::json;
struct ReportCleanup final {
    topology::ValidationReport& report;
    ~ReportCleanup() noexcept {
        for (auto& error : report.diagnostics)
            core::detail::retire_json(error.context);
    }
};
auto retain_lower(std::optional<core::Error>* sink, core::Error& error) noexcept
    -> const core::Error* {
    if (!sink)
        return &error;
    if (!*sink)
        sink->emplace(std::move(error));
    return &**sink;
}

auto admission_error(std::string classification, const core::Error* underlying = nullptr)
    -> core::Error {
    nlohmann::json context = core::detail::json_object<nlohmann::json>([&](auto& json_value_81) {
        core::detail::set_json(json_value_81, "classification", std::move(classification));
        core::detail::set_json(json_value_81, "component", nlohmann::json("runner_admission"));
    });
    core::detail::JsonCleanup context_cleanup{context};
    if (underlying != nullptr)
        context["underlying_code"] = underlying->code.value();
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.runner_state.admission_failed"},
            .message = "Runner placement admission failed",
            .operation = "runner_admission",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto pairwise_admission_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.pairwise_admission.incompatible_requests"},
            .message = "Admitted placement requests are not pairwise compatible",
            .operation = "pairwise_admission",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& json_value_82) {
                core::detail::set_json(json_value_82, "classification", std::move(classification));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

template <typename T, typename U>
auto equal_ranges(std::span<const T> first, std::span<const U> second) -> bool {
    return std::ranges::equal(first, second);
}

auto fixed_region_equal(const ipc::PlacementRegionRequest& first,
                        const ipc::PlacementRegionRequest& second) -> bool {
    return first.bytes() == second.bytes() && first.page_mode() == second.page_mode() &&
           first.page_size_bytes() == second.page_size_bytes() &&
           first.placement_threshold() == second.placement_threshold() &&
           first.prefault() == second.prefault() &&
           first.verify_before() == second.verify_before() &&
           first.verify_after() == second.verify_after();
}

auto placement_equal(const AdmittedWorker& first, const AdmittedWorker& second) -> bool {
    return first.request().region().policy() == second.request().region().policy() &&
           equal_ranges(first.request().region().nodes(), second.request().region().nodes()) &&
           equal_ranges(first.numa_nodes(), second.numa_nodes());
}

template <typename T> auto contains(std::span<const T> values, const T& value) -> bool {
    return std::find(values.begin(), values.end(), value) != values.end();
}

auto cpu_effective(const topology::TopologySnapshot& topology, core::LinuxCpuId cpu) -> bool {
    const auto& value = topology.machine().effective_cpus.value();
    return value && value->contains(cpu);
}

auto node_effective(const topology::TopologySnapshot& topology, core::LinuxNumaNodeId node)
    -> bool {
    const auto& value = topology.machine().effective_nodes.value();
    return value && value->contains(core::NumaNodeId{node.value()});
}

auto processing_unit_for_linux_cpu(const topology::TopologySnapshot& topology, core::LinuxCpuId cpu)
    -> std::optional<topology::ProcessingUnit> {
    const auto& pus = topology.processing_units();
    const auto found = std::find_if(pus.begin(), pus.end(),
                                    [cpu](const auto& pu) { return pu.linux_cpu_id == cpu; });
    if (found == pus.end())
        return std::nullopt;
    return *found;
}

auto placement_identity_projection(const ipc::PlacementRequest& request,
                                   const RunnerContext& context,
                                   std::span<const AdmittedWorker> workers,
                                   const AdmittedServiceReservation& service) -> nlohmann::json {
    auto worker_values = nlohmann::json::array();
    core::detail::JsonCleanup worker_values_cleanup{worker_values};
    for (std::size_t index = 0; index < workers.size(); ++index) {
        const auto& worker = workers[index];
        const auto& region = worker.request().region();
        std::vector<const ipc::PlacementNumaTarget*> canonical_targets;
        canonical_targets.reserve(region.nodes().size());
        for (const auto& target : region.nodes())
            canonical_targets.push_back(&target);
        std::ranges::sort(canonical_targets, {}, [](const auto* target) {
            return target->stable_numa_node_id();
        });
        auto nodes = nlohmann::json::array();
        core::detail::JsonCleanup nodes_cleanup{nodes};
        for (const auto* target : canonical_targets)
            core::detail::append_json(
                nodes, core::detail::json_object<Json>([&](auto& json_value_84) {
                    core::detail::set_json(json_value_84, "stable_numa_id",
                                           Json(target->stable_numa_node_id().value()));
                    core::detail::set_json(json_value_84, "linux_numa_node",
                                           Json(target->linux_numa_node().value()));
                }));
        core::detail::append_json(
            worker_values, core::detail::json_object<Json>([&](auto& json_value_30015) {
                core::detail::set_json(json_value_30015, "worker_id", Json(index));
                core::detail::set_json(json_value_30015, "stable_pu_id",
                                       Json(worker.request().cpu().stable_pu_id().value()));
                core::detail::set_json(json_value_30015, "stable_core_id",
                                       Json(worker.request().cpu().stable_core_id().value()));
                core::detail::set_json(json_value_30015, "linux_cpu",
                                       Json(worker.request().cpu().linux_cpu().value()));
                core::detail::set_json(
                    json_value_30015, "region",
                    core::detail::json_object<Json>([&](auto& json_value_30016) {
                        core::detail::set_json(json_value_30016, "bytes",
                                               Json(region.bytes().value()));
                        core::detail::set_json(json_value_30016, "policy", Json("bind"));
                        core::detail::set_json(json_value_30016, "nodes", std::move(nodes));
                        core::detail::set_json(json_value_30016, "page_mode", Json("base"));
                        core::detail::set_json(json_value_30016, "page_size_bytes",
                                               Json(region.page_size_bytes().value()));
                        core::detail::set_json(
                            json_value_30016, "placement_threshold",
                            core::detail::json_object<Json>([&](auto& json_value_30017) {
                                core::detail::set_json(
                                    json_value_30017, "numerator",
                                    Json(region.placement_threshold().numerator()));
                                core::detail::set_json(
                                    json_value_30017, "denominator",
                                    Json(region.placement_threshold().denominator()));
                            }));
                        core::detail::set_json(json_value_30016, "prefault",
                                               Json(region.prefault()));
                        core::detail::set_json(json_value_30016, "verify_before",
                                               Json(region.verify_before()));
                        core::detail::set_json(json_value_30016, "verify_after",
                                               Json(region.verify_after()));
                    }));
            }));
    }

    auto stable_cores = nlohmann::json::array();
    core::detail::JsonCleanup stable_cores_cleanup{stable_cores};
    for (const auto core : service.cores())
        stable_cores.push_back(context.stable_ids().core_id(core)->get().value());
    auto stable_pus = nlohmann::json::array();
    core::detail::JsonCleanup stable_pus_cleanup{stable_pus};
    for (const auto pu : service.effective_processing_units())
        stable_pus.push_back(context.stable_ids().processing_unit_id(pu)->get().value());
    auto linux_cpus = nlohmann::json::array();
    core::detail::JsonCleanup linux_cpus_cleanup{linux_cpus};
    for (const auto cpu : service.linux_cpus())
        linux_cpus.push_back(cpu.value());

    nlohmann::json result = nlohmann::json::object();
    core::detail::JsonCleanup result_cleanup{result};
    if (request.transport_version() == ipc::PlacementRequestTransportVersion::v4) {
        result["schema"] = "epycbench.admitted-placement-identity/v2";
        if (const auto* scenario =
                std::get_if<ScenarioPlacementProvenance>(&request.provenance())) {
            core::detail::set_json(
                result, "provenance", core::detail::json_object<Json>([&](auto& json_value_30011) {
                    core::detail::set_json(json_value_30011, "kind", Json("scenario"));
                    core::detail::set_json(json_value_30011, "resolved_scenario_plan_digest",
                                           Json(scenario->resolved_scenario_plan_digest().text()));
                    core::detail::set_json(json_value_30011, "scenario_digest",
                                           Json(scenario->scenario_digest().text()));
                }));
        } else {
            const auto& h =
                std::get<HostQualificationPlanPlacementProvenance>(request.provenance());
            core::detail::set_json(
                result, "provenance", core::detail::json_object<Json>([&](auto& json_value_30012) {
                    core::detail::set_json(json_value_30012, "environment_digest",
                                           Json(h.environment_digest().text()));
                    core::detail::set_json(json_value_30012, "host_qualification_plan_digest",
                                           Json(h.host_qualification_plan_digest().text()));
                    core::detail::set_json(json_value_30012, "kind",
                                           Json("host_qualification_plan"));
                    core::detail::set_json(json_value_30012, "policy_identity",
                                           Json(h.policy_identity().text()));
                    core::detail::set_json(json_value_30012, "template_sha256",
                                           Json(h.template_sha256().to_hex()));
                }));
        }
    } else {
        const auto& scenario = std::get<ScenarioPlacementProvenance>(request.provenance());
        result["schema"] = "epycbench.admitted-placement-identity/v1";
        result["scenario_digest"] = scenario.scenario_digest().text();
        result["resolved_scenario_plan_digest"] = scenario.resolved_scenario_plan_digest().text();
    }
    core::detail::set_json(
        result, "topology", core::detail::json_object<Json>([&](auto& json_value_30013) {
            core::detail::set_json(json_value_30013, "physical_fingerprint",
                                   Json(request.topology().physical_fingerprint().to_string()));
            core::detail::set_json(json_value_30013, "effective_fingerprint",
                                   Json(request.topology().effective_fingerprint().to_string()));
        }));
    result["workers"] = std::move(worker_values);
    core::detail::set_json(
        result, "service_reservation", core::detail::json_object<Json>([&](auto& json_value_30014) {
            core::detail::set_json(json_value_30014, "reserved_stable_core_ids",
                                   std::move(stable_cores));
            core::detail::set_json(json_value_30014, "reserved_stable_pu_ids",
                                   std::move(stable_pus));
            core::detail::set_json(json_value_30014, "reserved_linux_cpus", std::move(linux_cpus));
        }));
    return Json(std::move(result));
}

} // namespace

namespace detail {

struct AdmissionFactory {
    static auto identity(AdmittedPlacementIdentityVersion version, core::Sha256Digest digest) -> AdmittedPlacementIdentity {
        return AdmittedPlacementIdentity{version,digest};
    }
    static auto worker(const ipc::PlacementWorkerRequest& request, core::ProcessingUnitId pu,
                       core::CoreId core, std::vector<core::NumaNodeId> nodes) -> AdmittedWorker {
        return {request, pu, core, std::move(nodes)};
    }
    static auto service(std::vector<core::CoreId> cores,
                        std::vector<core::ProcessingUnitId> effective_pus,
                        std::vector<core::LinuxCpuId> linux_cpus) -> AdmittedServiceReservation {
        return {std::move(cores), std::move(effective_pus), std::move(linux_cpus)};
    }
    static auto request(ipc::RequestId request_id, const ipc::PlacementRequest& request,
                        const RunnerContext& context, std::vector<AdmittedWorker> workers,
                        AdmittedServiceReservation service,
                        AdmittedPlacementIdentity identity) -> AdmittedPlacementRequest {
        return {request_id,
                request,
                context,
                std::move(workers),
                std::move(service),
                AdmittedExecutionInputs{request.execution_inputs()},
                std::move(identity)};
    }
};

} // namespace detail

auto detail::PairwiseV2AdmissionClassificationAccess::admit_placement(
    ipc::RequestId request_id, const ipc::PlacementRequest& request,
    const RunnerContext& context) noexcept
    -> std::expected<AdmittedPlacementRequest,
                     detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
    const auto fail = [](detail::PairwiseV2ContextualAdmissionBranch branch,
                         core::Error error)
        -> std::expected<AdmittedPlacementRequest,
                         detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            branch, std::move(error)}};
    };
    using Branch = detail::PairwiseV2ContextualAdmissionBranch;
    try {
        return admit_placement_fallible(request_id, request, context);
    } catch (const std::bad_alloc&) {
        return fail(Branch::allocation, admission_error("contained_exception"));
    } catch (const std::exception&) {
        return fail(Branch::placement_contained_exception, admission_error("contained_exception"));
    } catch (...) {
        return fail(Branch::placement_contained_unknown_failure,
                    admission_error("contained_unknown_failure"));
    }
}
auto detail::PairwiseV2AdmissionClassificationAccess::admit_placement_fallible(
    ipc::RequestId request_id, const ipc::PlacementRequest& request, const RunnerContext& context,
    std::optional<core::Error>* raw_failure)
    -> std::expected<AdmittedPlacementRequest, detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
    const auto fail =
        [](detail::PairwiseV2ContextualAdmissionBranch branch,
           core::Error error) -> std::expected<AdmittedPlacementRequest,
                                               detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
        return std::unexpected{
            detail::PairwiseV2LegacyClassifiedAdmissionFailure{branch, std::move(error)}};
    };
    using Branch = detail::PairwiseV2ContextualAdmissionBranch;
    const auto& topology = context.topology();
    auto report = topology::validate_topology(topology);
    ReportCleanup report_cleanup{report};
    if (!report.valid()) {
        if (raw_failure && !raw_failure->has_value() && !report.diagnostics.empty())
            raw_failure->emplace(std::move(report.diagnostics.front()));
        return fail(Branch::placement_context_invariant,
                    admission_error("context_invariant_failed"));
    }
    auto derived = topology::derive_topology_fingerprints(topology, context.stable_ids());
    core::detail::ExpectedErrorCleanup derived_cleanup{derived};
    const auto* derived_error = derived ? nullptr : retain_lower(raw_failure, derived.error());
    if (!derived || *derived != context.fingerprints())
        return fail(Branch::placement_context_invariant,
                    admission_error("context_invariant_failed", derived_error));

    if (request.topology().physical_fingerprint() != context.fingerprints().physical)
        return fail(Branch::placement_topology_mismatch,
                    admission_error("physical_fingerprint_mismatch"));
    if (request.topology().effective_fingerprint() != context.fingerprints().effective)
        return fail(Branch::placement_topology_mismatch,
                    admission_error("effective_fingerprint_mismatch"));

    const auto kernel_working_set = std::visit(
        [](const auto& inputs) {
            using Inputs = std::remove_cvref_t<decltype(inputs)>;
            if constexpr (std::is_same_v<Inputs, ipc::PlacementBandwidthExecutionInputs>)
                return inputs.configuration().working_set_per_worker();
            else
                return inputs.configuration().working_set();
        },
        request.execution_inputs().kernel());
    for (const auto& worker : request.workers())
        if (worker.region().bytes() != kernel_working_set)
            return fail(Branch::placement_policy_mismatch, admission_error("working_set_mismatch"));

    struct ResolvedWorker {
        const ipc::PlacementWorkerRequest* request;
        core::ProcessingUnitId pu;
        core::CoreId core;
    };
    std::vector<ResolvedWorker> resolved_workers;
    resolved_workers.reserve(request.workers().size());
    std::set<core::ProcessingUnitId> worker_pus;
    std::set<core::LinuxCpuId> worker_cpus;

    for (const auto& worker : request.workers()) {
        const auto pu_id = context.stable_ids().processing_unit(worker.cpu().stable_pu_id());
        if (!pu_id)
            return fail(Branch::placement_present_cpu_mismatch,
                        admission_error("stable_linux_cpu_mismatch"));
        const auto core_id = context.stable_ids().core(worker.cpu().stable_core_id());
        if (!core_id)
            return fail(Branch::placement_topology_mismatch,
                        admission_error("stable_core_membership_mismatch"));
        const auto pu = topology.processing_unit(*pu_id);
        const auto core = topology.core(*core_id);
        if (!pu || pu->linux_cpu_id != worker.cpu().linux_cpu())
            return fail(Branch::placement_present_cpu_mismatch,
                        admission_error("stable_linux_cpu_mismatch"));
        if (!core || pu->core_id != *core_id)
            return fail(Branch::placement_topology_mismatch,
                        admission_error("stable_core_membership_mismatch"));
        if (!cpu_effective(topology, worker.cpu().linux_cpu()))
            return fail(Branch::placement_present_cpu_mismatch,
                        admission_error("resource_not_effective"));
        if (!worker_pus.insert(*pu_id).second ||
            !worker_cpus.insert(worker.cpu().linux_cpu()).second)
            return fail(Branch::placement_policy_mismatch,
                        admission_error("conflicting_assignment"));
        resolved_workers.push_back({&worker, *pu_id, *core_id});
    }

    std::vector<AdmittedWorker> workers;
    workers.reserve(resolved_workers.size());
    for (const auto& resolved : resolved_workers) {
        const auto& worker = *resolved.request;
        std::vector<core::NumaNodeId> nodes;
        nodes.reserve(worker.region().nodes().size());
        std::set<core::NumaNodeId> unique_nodes;
        for (const auto& target : worker.region().nodes()) {
            const auto node_id = context.stable_ids().numa_node(target.stable_numa_node_id());
            if (!node_id)
                return fail(Branch::placement_topology_mismatch,
                            admission_error("stable_linux_numa_mismatch"));
            const auto node = topology.numa_node(*node_id);
            if (!node || !node->linux_node_id.value() ||
                *node->linux_node_id.value() != target.linux_numa_node().value())
                return fail(Branch::placement_topology_mismatch,
                            admission_error("stable_linux_numa_mismatch"));
            if (!node_effective(topology, target.linux_numa_node()))
                return fail(Branch::placement_topology_mismatch,
                            admission_error("resource_not_effective"));
            if (!contains<core::ByteCount>(node->page_sizes, worker.region().page_size_bytes()))
                return fail(Branch::placement_topology_mismatch,
                            admission_error("resource_not_effective"));
            if (!unique_nodes.insert(*node_id).second)
                return fail(Branch::placement_policy_mismatch,
                            admission_error("conflicting_assignment"));
            nodes.push_back(*node_id);
        }
        workers.push_back(
            detail::AdmissionFactory::worker(worker, resolved.pu, resolved.core, std::move(nodes)));
    }

    std::vector<core::CoreId> service_cores;
    service_cores.reserve(request.reserved_service().stable_cores().size());
    std::set<core::CoreId> service_core_set;
    for (const auto& stable_core : request.reserved_service().stable_cores()) {
        const auto core_id = context.stable_ids().core(stable_core);
        if (!core_id || !topology.core(*core_id) || !service_core_set.insert(*core_id).second)
            return fail(Branch::placement_policy_mismatch,
                        admission_error("service_reservation_mismatch"));
        service_cores.push_back(*core_id);
    }

    std::set<core::LinuxCpuId> service_cpu_set;
    for (const auto cpu : request.reserved_service().linux_cpus()) {
        if (!cpu_effective(topology, cpu))
            return fail(Branch::placement_present_cpu_mismatch,
                        admission_error("resource_not_effective"));
        const auto pu = processing_unit_for_linux_cpu(topology, cpu);
        if (!pu || !service_core_set.contains(pu->core_id))
            return fail(Branch::placement_policy_mismatch,
                        admission_error("service_reservation_mismatch"));
        if (!service_cpu_set.insert(cpu).second)
            return fail(Branch::placement_policy_mismatch,
                        admission_error("conflicting_assignment"));
    }

    std::vector<core::ProcessingUnitId> effective_service_pus;
    for (const auto core_id : service_cores) {
        for (const auto pu_id : topology.core_members(core_id)) {
            const auto pu = topology.processing_unit(pu_id);
            if (!pu)
                return fail(Branch::placement_context_invariant,
                            admission_error("context_invariant_failed"));
            if (cpu_effective(topology, pu->linux_cpu_id)) {
                if (!service_cpu_set.contains(pu->linux_cpu_id))
                    return fail(Branch::placement_policy_mismatch,
                                admission_error("service_reservation_mismatch"));
                effective_service_pus.push_back(pu_id);
            }
        }
    }

    for (const auto& worker : workers) {
        if (service_cpu_set.contains(worker.request().cpu().linux_cpu()) ||
            service_core_set.contains(worker.core()))
            return fail(Branch::placement_policy_mismatch,
                        admission_error("conflicting_assignment"));
    }

    std::vector<core::LinuxCpuId> service_cpus(request.reserved_service().linux_cpus().begin(),
                                               request.reserved_service().linux_cpus().end());
    auto service = detail::AdmissionFactory::service(
        std::move(service_cores), std::move(effective_service_pus), std::move(service_cpus));
    auto identity = placement_identity_projection(request, context, workers, service);
    core::detail::JsonCleanup identity_cleanup{identity};
    auto identity_digest = core::hash_canonical_json(identity);
    core::detail::ExpectedErrorCleanup identity_digest_cleanup{identity_digest};
    if (!identity_digest) {
        const auto* underlying = retain_lower(raw_failure, identity_digest.error());
        return fail(Branch::placement_identity_failure,
                    admission_error("placement_identity_failed", underlying));
    }
    return detail::AdmissionFactory::request(
        request_id, request, context, std::move(workers), std::move(service),
        detail::AdmissionFactory::identity(request.transport_version() ==
                                                   ipc::PlacementRequestTransportVersion::v4
                                               ? AdmittedPlacementIdentityVersion::v2
                                               : AdmittedPlacementIdentityVersion::v1,
                                           *identity_digest));
}

detail::PairwiseV2LegacyClassifiedAdmissionFailure::
    ~PairwiseV2LegacyClassifiedAdmissionFailure() noexcept {
    core::detail::retire_json(legacy_.context);
}
auto detail::PairwiseV2AdmissionClassificationAccess::admit_placement_source(
    ipc::RequestId id, const ipc::PlacementRequest& request, const RunnerContext& context,
    std::optional<core::Error>* raw_failure)
    -> std::expected<AdmittedPlacementRequest, core::Error> {
    auto result = admit_placement_fallible(id, request, context, raw_failure);
    if (!result)
        return std::unexpected{std::move(result.error().legacy_)};
    return std::move(*result);
}

auto detail::PairwiseV2AdmissionClassificationAccess::admit_pair(
    AdmittedPlacementRequest first, AdmittedPlacementRequest second,
    std::uint64_t resolved_comparison_order_seed) noexcept
    -> std::expected<AdmittedPairwiseProductionRequest,
                     detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
    using Branch = detail::PairwiseV2ContextualAdmissionBranch;
    const auto fail = [](Branch branch, std::string classification)
        -> std::expected<AdmittedPairwiseProductionRequest,
                         detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            branch, pairwise_admission_error(std::move(classification))}};
    };

    try {
    if (&first.context() != &second.context())
        return fail(Branch::pair_context_mismatch, "context_mismatch");
    auto first_scenario = first.request().scenario_provenance();
    if (!first_scenario)
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::pair_provenance_mismatch, first_scenario.error()}};
    auto second_scenario = second.request().scenario_provenance();
    if (!second_scenario)
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::pair_provenance_mismatch, second_scenario.error()}};
    const auto wrong_transport = [] {
        return core::Error{
            .category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.placement_provenance.wrong_transport"},
            .message = "General v4 execution is not authorized",
            .operation = "admit_pairwise_production_request",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& json_value_83) {
                core::detail::set_json(json_value_83, "classification",
                                       nlohmann::json("general_v4_execution_not_authorized"));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
    };
    if (first.request().transport_version() == ipc::PlacementRequestTransportVersion::v4)
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::pair_transport_mismatch, wrong_transport()}};
    if (second.request().transport_version() == ipc::PlacementRequestTransportVersion::v4)
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::pair_transport_mismatch, wrong_transport()}};
    if (first_scenario->get().scenario_digest() != second_scenario->get().scenario_digest())
        return fail(Branch::pair_provenance_mismatch, "scenario_identity_mismatch");
    if (first_scenario->get().resolved_scenario_plan_digest() !=
        second_scenario->get().resolved_scenario_plan_digest())
        return fail(Branch::pair_provenance_mismatch, "resolved_plan_mismatch");
    if (first.request().topology() != second.request().topology())
        return fail(Branch::pair_topology_mismatch, "topology_request_mismatch");
    if (first.request().experiment() != second.request().experiment())
        return fail(Branch::pair_policy_mismatch, "experiment_mismatch");
    if (first.execution_inputs().kernel() != second.execution_inputs().kernel())
        return fail(Branch::pair_policy_mismatch, "kernel_execution_inputs_mismatch");
    if (first.execution_inputs().measurement() != second.execution_inputs().measurement())
        return fail(Branch::pair_policy_mismatch, "measurement_inputs_mismatch");
    if (first.workers().size() != second.workers().size())
        return fail(Branch::pair_policy_mismatch, "worker_count_mismatch");

    for (std::size_t index = 0; index < first.workers().size(); ++index) {
        const auto& first_worker = first.workers()[index];
        const auto& second_worker = second.workers()[index];
        if (first_worker.processing_unit() != second_worker.processing_unit() ||
            first_worker.core() != second_worker.core() ||
            first_worker.request().cpu() != second_worker.request().cpu())
            return fail(Branch::pair_policy_mismatch, "worker_identity_mismatch");
    }

    for (std::size_t index = 0; index < first.workers().size(); ++index)
        if (!fixed_region_equal(first.workers()[index].request().region(),
                                second.workers()[index].request().region()))
            return fail(Branch::pair_policy_mismatch, "worker_region_invariant_mismatch");

    if (first.request().reserved_service() != second.request().reserved_service() ||
        !equal_ranges(first.service().cores(), second.service().cores()) ||
        !equal_ranges(first.service().effective_processing_units(),
                      second.service().effective_processing_units()) ||
        !equal_ranges(first.service().linux_cpus(), second.service().linux_cpus()))
        return fail(Branch::pair_policy_mismatch, "service_reservation_mismatch");

    const auto placement_distinct = [&] {
        for (std::size_t index = 0; index < first.workers().size(); ++index)
            if (!placement_equal(first.workers()[index], second.workers()[index]))
                return true;
        return false;
    }();
    if (!placement_distinct)
        return fail(Branch::pair_policy_mismatch, "placement_factor_not_distinct");

    return AdmittedPairwiseProductionRequest{std::move(first), std::move(second),
                                              resolved_comparison_order_seed};
    } catch (const std::bad_alloc&) {
        return fail(Branch::allocation, "contained_exception");
    } catch (const std::exception&) {
        return fail(Branch::production_contained_exception, "contained_exception");
    } catch (...) {
        return fail(Branch::production_contained_unknown_failure,
                    "contained_unknown_failure");
    }
}

auto admit_placement_request(ipc::RequestId request_id, const ipc::PlacementRequest& request,
                             const RunnerContext& context) noexcept
    -> std::expected<AdmittedPlacementRequest, core::Error> {
    auto result = detail::PairwiseV2AdmissionClassificationAccess::admit_placement(
        request_id, request, context);
    if (!result)
        return std::unexpected{std::move(result.error().legacy_)};
    return std::move(*result);
}

auto AdmittedPairwiseProductionRequest::create(
    AdmittedPlacementRequest first, AdmittedPlacementRequest second,
    std::uint64_t resolved_comparison_order_seed) noexcept
    -> std::expected<AdmittedPairwiseProductionRequest, core::Error> {
    auto result = detail::PairwiseV2AdmissionClassificationAccess::admit_pair(
        std::move(first), std::move(second), resolved_comparison_order_seed);
    if (!result)
        return std::unexpected{std::move(result.error().legacy_)};
    return std::move(*result);
}

} // namespace epycbench::runtime
