#include "epycbench/monitoring/production_admission_projection.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <algorithm>
#include <exception>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>

namespace epycbench::monitoring::qualification {
namespace {
auto error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code =
                core::StableErrorCode{"monitoring.production_admission_projection.invalid_input"},
            .message = "MON-007 production admission facts are inconsistent",
            .operation = "monitoring.project_production_admission",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

template <class T> auto text(const T& id) { return std::string{id.value()}; }

struct SelectedVariantView {
    const runtime::AdmittedPlacementRequest& admitted;
    const runtime::AdmittedPlacementRequest& other;
    std::string_view canonical_role;
};

auto select_pairwise_variant(const runtime::AdmittedPairwiseProductionInputs& inputs,
                             runtime::PairwiseVariant variant)
    -> std::expected<SelectedVariantView, core::Error> {
    const auto& pair = inputs.pairwise_request();
    switch (variant) {
    case runtime::PairwiseVariant::first:
        return SelectedVariantView{pair.first(), pair.second(), "first"};
    case runtime::PairwiseVariant::second:
        return SelectedVariantView{pair.second(), pair.first(), "second"};
    default:
        return std::unexpected{error("invalid_variant")};
    }
}

auto select_pairwise_variant(const runtime::CalibrationOnlyProductionAdmission& admission,
                             runtime::PairwiseVariant variant)
    -> std::expected<SelectedVariantView, core::Error> {
    switch (variant) {
    case runtime::PairwiseVariant::first:
        return SelectedVariantView{admission.first(), admission.second(), "first"};
    case runtime::PairwiseVariant::second:
        return SelectedVariantView{admission.second(), admission.first(), "second"};
    default:
        return std::unexpected{error("invalid_variant")};
    }
}

} // namespace

template <class PairAuthority>
auto project_impl(const topology::ReferenceFixtureAuthority& fixture,
                  const PairAuthority& inputs, runtime::PairwiseVariant variant) noexcept
    -> std::expected<MonitoringProductionAdmissionProjection, core::Error> {
    try {
        const auto selected = select_pairwise_variant(inputs, variant);
        if (!selected)
            return std::unexpected{selected.error()};
        if (fixture.fixture_id() != "reference-epyc" || fixture.fixture_version() != "1")
            return std::unexpected{error("fixture_identity")};
        auto fixture_ids = topology::derive_stable_topology_ids(fixture.topology());
        auto fixture_fingerprints =
            fixture_ids ? topology::derive_topology_fingerprints(fixture.topology(), *fixture_ids)
                        : std::expected<topology::TopologyFingerprints, core::Error>{
                              std::unexpected{error("fixture_stable_ids")}};
        if (!fixture_ids || !fixture_fingerprints)
            return std::unexpected{error("fixture_topology")};
        const auto& admitted = selected->admitted;
        auto scenario = admitted.request().scenario_provenance();
        if (!scenario)
            return std::unexpected{error("scenario_provenance_required")};
        const auto& other = selected->other;
        if (!std::ranges::equal(admitted.service().cores(), other.service().cores()) ||
            !std::ranges::equal(admitted.service().effective_processing_units(),
                                other.service().effective_processing_units()) ||
            !std::ranges::equal(admitted.service().linux_cpus(), other.service().linux_cpus()))
            return std::unexpected{error("pairwise_service_reservation")};
        const auto& context = admitted.context();
        if (context.fingerprints() != *fixture_fingerprints)
            return std::unexpected{error("topology_fingerprint")};
        std::vector<topology::StableNumaNodeId> order;
        for (const auto& entry : fixture_ids->numa_nodes())
            order.push_back(entry.semantic_id);
        std::ranges::sort(order);
        if (order.size() != 8U)
            return std::unexpected{error("reference_numa_count")};
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i].value() != "numa/" + std::to_string(i))
                return std::unexpected{error("reference_numa_order")};
        const auto expected_worker_count =
            std::holds_alternative<runtime::ipc::PlacementLatencyExecutionInputs>(
                admitted.request().execution_inputs().kernel())
                ? 1U
                : 8U;
        if (admitted.workers().size() != expected_worker_count)
            return std::unexpected{error("worker_count")};

        std::vector<AdmittedWorkerFact> workers;
        std::set<core::CoreId> worker_cores;
        std::set<core::LinuxCpuId> worker_cpus;
        std::set<std::string> worker_nodes;
        bool worker_local_memory = true;
        for (std::size_t i = 0; i < admitted.workers().size(); ++i) {
            const auto& worker = admitted.workers()[i];
            if (worker.numa_nodes().size() != 1U)
                return std::unexpected{error("worker_memory_target")};
            const auto pu = context.topology().processing_unit(worker.processing_unit());
            if (!pu || pu->core_id != worker.core() ||
                pu->linux_cpu_id != worker.request().cpu().linux_cpu())
                return std::unexpected{error("worker_topology")};
            const auto cpu_nodes = context.topology().numa_nodes_for_pu(worker.processing_unit());
            if (cpu_nodes.size() != 1U)
                return std::unexpected{error("worker_numa")};
            const auto stable_numa = context.stable_ids().numa_node_id(cpu_nodes.front());
            const auto stable_memory_numa =
                context.stable_ids().numa_node_id(worker.numa_nodes().front());
            const auto stable_core = context.stable_ids().core_id(worker.core());
            const auto stable_pu =
                context.stable_ids().processing_unit_id(worker.processing_unit());
            if (!stable_numa || !stable_memory_numa || !stable_core || !stable_pu ||
                stable_core->get() != worker.request().cpu().stable_core_id() ||
                stable_pu->get() != worker.request().cpu().stable_pu_id())
                return std::unexpected{error("worker_stable_identity")};
            const auto& region_nodes = worker.request().region().nodes();
            if (region_nodes.size() != 1U ||
                region_nodes.front().stable_numa_node_id() != stable_memory_numa->get())
                return std::unexpected{error("worker_memory_target")};
            worker_local_memory = worker_local_memory &&
                                  stable_memory_numa->get() == stable_numa->get();
            workers.push_back({core::WorkerId{static_cast<std::uint32_t>(i)}, stable_numa->get(),
                               stable_core->get(), stable_pu->get(), pu->linux_cpu_id,
                               stable_memory_numa->get()});
            worker_cores.insert(worker.core());
            worker_cpus.insert(pu->linux_cpu_id);
            worker_nodes.insert(text(stable_numa->get()));
        }
        if (worker_cores.size() != expected_worker_count ||
            worker_cpus.size() != expected_worker_count ||
            worker_nodes.size() != expected_worker_count)
            return std::unexpected{error("worker_uniqueness")};
        for (std::size_t i = 0; i < workers.size(); ++i)
            if (workers[i].numa.value() != order[i].value())
                return std::unexpected{error("worker_order")};

        ServiceReservationFact service;
        for (auto id : admitted.service().cores()) {
            if (worker_cores.contains(id))
                return std::unexpected{error("worker_service_core_overlap")};
            auto stable = context.stable_ids().core_id(id);
            if (!stable)
                return std::unexpected{error("service_stable_core")};
            service.reserved_cores.push_back(stable->get());
        }
        for (auto id : admitted.service().effective_processing_units()) {
            auto stable = context.stable_ids().processing_unit_id(id);
            if (!stable)
                return std::unexpected{error("service_stable_pu")};
            service.reserved_processing_units.push_back(stable->get());
        }
        for (auto cpu : admitted.service().linux_cpus()) {
            if (worker_cpus.contains(cpu))
                return std::unexpected{error("worker_service_cpu_overlap")};
            service.reserved_linux_cpus.push_back(cpu);
        }
        const AdmissionInvariantFacts invariants{true, true, true, true, worker_local_memory, true};
        nlohmann::json json{
            {"schema", monitoring_admitted_production_facts_schema},
            {"variant_role", selected->canonical_role},
            {"topology",
             {{"fixture_id", fixture.fixture_id()},
              {"fixture_version", fixture.fixture_version()},
              {"manifest_sha256", fixture.manifest_sha256().to_hex()},
              {"checksums_sha256", fixture.checksums_sha256().to_hex()},
              {"physical_fingerprint", fixture_fingerprints->physical.to_string()},
              {"effective_fingerprint", fixture_fingerprints->effective.to_string()}}},
            {"admitted_placement_identity", admitted.placement_identity().to_string()},
            {"scenario_digest", scenario->get().scenario_digest().text()},
            {"resolved_scenario_plan_digest",
             scenario->get().resolved_scenario_plan_digest().text()}};
        json["topology"]["stable_numa_order"] = nlohmann::json::array();
        for (const auto& id : order)
            json["topology"]["stable_numa_order"].push_back(id.value());
        json["workers"] = nlohmann::json::array();
        for (const auto& w : workers)
            json["workers"].push_back(
                {{"worker_id", w.worker_id.value()},
                 {"stable_numa_id", w.numa.value()},
                 {"stable_core_id", w.core.value()},
                 {"stable_pu_id", w.processing_unit.value()},
                 {"linux_cpu", w.linux_cpu.value()},
                 {"memory_target_stable_numa_id", w.memory_target_numa.value()}});
        auto& sj = json["service_reservation"];
        sj = {{"reserved_stable_core_ids", nlohmann::json::array()},
              {"reserved_stable_pu_ids", nlohmann::json::array()},
              {"reserved_linux_cpus", nlohmann::json::array()}};
        for (const auto& id : service.reserved_cores)
            sj["reserved_stable_core_ids"].push_back(id.value());
        for (const auto& id : service.reserved_processing_units)
            sj["reserved_stable_pu_ids"].push_back(id.value());
        for (auto id : service.reserved_linux_cpus)
            sj["reserved_linux_cpus"].push_back(id.value());
        json["admission_invariants"] = {{"one_worker_per_required_numa_node", true},
                                        {"unique_workers", true},
                                        {"worker_service_core_disjoint", true},
                                        {"worker_service_cpu_disjoint", true},
                                        {"worker_local_memory", worker_local_memory},
                                        {"topology_consistent", true}};
        auto canonical = core::canonicalize_json(json);
        if (!canonical)
            return std::unexpected{canonical.error()};
        auto digest = core::sha256(*canonical);
        return MonitoringProductionAdmissionProjection{
            variant,
            std::string{fixture.fixture_id()},
            std::string{fixture.fixture_version()},
            fixture.manifest_sha256(),
            fixture.checksums_sha256(),
            fixture_fingerprints->physical,
            fixture_fingerprints->effective,
            std::move(order),
            std::move(workers),
            std::move(service),
            admitted.placement_identity(),
            scenario->get().scenario_digest().text(),
            scenario->get().resolved_scenario_plan_digest().text(),
            invariants,
            std::move(*canonical),
            digest};
    } catch (const std::exception&) {
        return std::unexpected{error("contained_exception")};
    } catch (...) {
        return std::unexpected{error("contained_unknown_failure")};
    }
}

auto project_monitoring_production_admission(const topology::ReferenceFixtureAuthority& fixture,
                                             const runtime::AdmittedPairwiseProductionInputs& inputs,
                                             runtime::PairwiseVariant variant) noexcept
    -> std::expected<MonitoringProductionAdmissionProjection, core::Error> {
    return project_impl(fixture, inputs, variant);
}

auto project_monitoring_production_admission(
    const topology::ReferenceFixtureAuthority& fixture,
    const runtime::CalibrationOnlyProductionAdmission& admission,
    runtime::PairwiseVariant variant) noexcept
    -> std::expected<MonitoringProductionAdmissionProjection, core::Error> {
    return project_impl(fixture, admission, variant);
}
} // namespace epycbench::monitoring::qualification
