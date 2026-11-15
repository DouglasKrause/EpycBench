#include "runtime/pairwise_monitoring_host_capacity_admission.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <ranges>
#include <set>
#include <variant>

namespace epycbench::runtime::detail {
namespace {
using Failure = PairwiseMonitoringHostCapacityAdmissionFailure;
using LowerAccess = platform_linux::detail::LinuxMonitoringCapacityAuthorityConstruction;

auto base_page(const RunnerContext& context) noexcept -> std::optional<core::ByteCount> {
    const auto& values = context.topology().machine().page_sizes;
    if (values.empty())
        return std::nullopt;
    const auto found = std::ranges::min_element(values);
    if (found == values.end() || found->value() == 0U)
        return std::nullopt;
    return *found;
}
auto present_cpus(const RunnerContext& context) -> std::vector<core::LinuxCpuId> {
    const auto& source = context.topology().machine().present_cpus;
    std::vector<core::LinuxCpuId> result;
    result.reserve(source.cardinality());
    for (const auto cpu : source)
        result.push_back(cpu);
    return result;
}
auto disposition(const ipc::PlacementMonitoringInputs& value,
                 ipc::PlacementMonitoringChannel channel) noexcept
    -> monitoring::MonitoringChannelDisposition {
    if (std::ranges::find(value.required(), channel) != value.required().end())
        return monitoring::MonitoringChannelDisposition::required;
    if (std::ranges::find(value.optional(), channel) != value.optional().end())
        return monitoring::MonitoringChannelDisposition::optional;
    return monitoring::MonitoringChannelDisposition::disabled;
}
auto unavailable(const ipc::PlacementMonitoringInputs& value) noexcept
    -> monitoring::OptionalUnavailableSourcePolicy {
    switch (value.unavailable_optional_source()) {
    case ipc::PlacementOptionalUnavailableSourcePolicy::fail:
        return monitoring::OptionalUnavailableSourcePolicy::fail;
    case ipc::PlacementOptionalUnavailableSourcePolicy::warn:
        return monitoring::OptionalUnavailableSourcePolicy::warn;
    case ipc::PlacementOptionalUnavailableSourcePolicy::disable:
        return monitoring::OptionalUnavailableSourcePolicy::disable;
    }
    return monitoring::OptionalUnavailableSourcePolicy::fail;
}
auto matches_policy(const ipc::PlacementMonitoringInputs& value,
                    const monitoring::MonitoringExecutionPolicy& policy) noexcept -> bool {
    constexpr std::array pairs{
        std::pair{ipc::PlacementMonitoringChannel::page_faults,
                  monitoring::MonitoringChannel::page_faults},
        std::pair{ipc::PlacementMonitoringChannel::context_switches,
                  monitoring::MonitoringChannel::context_switches},
        std::pair{ipc::PlacementMonitoringChannel::temperature,
                  monitoring::MonitoringChannel::temperature},
        std::pair{ipc::PlacementMonitoringChannel::frequency,
                  monitoring::MonitoringChannel::frequency}};
    return std::ranges::all_of(pairs, [&](const auto& pair) {
               return disposition(value, pair.first) == policy.disposition(pair.second);
           }) &&
           value.before_observation() == policy.before_observation() &&
           value.after_observation() == policy.after_observation() &&
           value.during_observation() == policy.during_observation() &&
           value.during_interval() == policy.during_interval() &&
           unavailable(value) == policy.optional_unsupported();
}

auto single_eligible(const AdmittedPlacementRequest& input) noexcept -> bool {
    return !input.workers().empty() &&
           input.request().transport_version() == ipc::PlacementRequestTransportVersion::v4 &&
           std::holds_alternative<HostQualificationPlanPlacementProvenance>(input.request().provenance()) &&
           !input.execution_inputs().measurement().calibration_enabled();
}
template <typename T> auto same_members(std::span<const T> left, std::span<const T> right) noexcept -> bool {
    return left.size() == right.size() && std::ranges::all_of(left, [&](const T& value) {
        return std::ranges::count(left, value) == 1 && std::ranges::find(right, value) != right.end();
    });
}
auto single_service_matches(const AdmittedPlacementRequest& input,
                            const ServicePlacementPlan& service, bool asynchronous,
                            std::span<const core::LinuxCpuId> workers,
                            std::span<const core::LinuxCpuId> present) noexcept -> bool {
    const auto& reservation = input.service();
    if (!same_members(service.reserved_cores(), reservation.cores()) ||
        !same_members(service.reserved_processing_units(), reservation.effective_processing_units()) ||
        !same_members(service.reserved_linux_cpus(), reservation.linux_cpus()) ||
        service.assignments().size() != (asynchronous ? 2U : 1U)) return false;
    bool control{}, measurement{};
    std::optional<core::CoreId> control_core, measurement_core;
    for (const auto& assignment : service.assignments()) {
        if (assignment.role == ServiceRole::control_monitor) {
            if (control) return false;
            control = true; control_core = assignment.core;
        } else if (assignment.role == ServiceRole::measurement_monitor) {
            if (measurement) return false;
            measurement = true; measurement_core = assignment.core;
        } else return false;
        const auto& context = input.context();
        const auto pu = std::ranges::find(context.topology().processing_units(), assignment.processing_unit,
                                         &topology::ProcessingUnit::id);
        const auto stable_pu = context.stable_ids().processing_unit_id(assignment.processing_unit);
        const auto stable_core = context.stable_ids().core_id(assignment.core);
        if (pu == context.topology().processing_units().end() || !stable_pu || !stable_core ||
            pu->core_id != assignment.core || pu->linux_cpu_id != assignment.linux_cpu ||
            stable_pu->get() != assignment.stable_pu || stable_core->get() != assignment.stable_core ||
            std::ranges::find(reservation.cores(), assignment.core) == reservation.cores().end() ||
            std::ranges::find(reservation.effective_processing_units(), assignment.processing_unit) == reservation.effective_processing_units().end() ||
            std::ranges::find(reservation.linux_cpus(), assignment.linux_cpu) == reservation.linux_cpus().end() ||
            std::ranges::find(present, assignment.linux_cpu) == present.end() ||
            std::ranges::find(workers, assignment.linux_cpu) != workers.end() ||
            std::ranges::any_of(input.workers(), [&](const auto& worker) { return worker.core() == assignment.core; }))
            return false;
    }
    return control && measurement == asynchronous && (!asynchronous || control_core != measurement_core);
}
} // namespace

auto PairwiseMonitoringHostCapacityAdmissionAccess::mint_single(const AdmittedPlacementRequest& input) noexcept
    -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    if (!single_eligible(input))
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::invalid_host_facts}};
    return mint_request(input.context(), input.request());
}
auto PairwiseMonitoringHostCapacityAdmissionAccess::mint_single_with(
    const AdmittedPlacementRequest& input,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations operations) noexcept
    -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    if (!single_eligible(input))
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::invalid_host_facts}};
    return mint_request_with(input.context(), input.request(), operations);
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::project_single(
    const AdmittedPlacementRequest& input, const monitoring::MonitoringExecutionPolicy& policy,
    const ServicePlacementPlan& service,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority controller,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority runner,
    const core::Sha256Digest& capacity_identity, const core::Sha256Digest& profile_identity) noexcept
    -> std::expected<PairwiseMonitoringHostCapacityAdmission, Failure> {
    try {
        if (!controller.authentically_bound()) return std::unexpected{Failure::controller_unbound};
        if (!runner.authentically_bound()) return std::unexpected{Failure::runner_unbound};
        if (!controller.stable_equals(runner)) return std::unexpected{Failure::lower_authority_mismatch};
        if (capacity_identity != expected_capacity_identity())
            return std::unexpected{Failure::capacity_contract_identity_mismatch};
        if (profile_identity != expected_profile_identity())
            return std::unexpected{Failure::finalized_error_profile_identity_mismatch};
        if (!single_eligible(input)) return std::unexpected{Failure::invalid_input};
        const auto source = provenance_for(input.request().provenance());
        if (!source || controller.provenance() != *source) return std::unexpected{Failure::provenance_mismatch};
        const auto& context = input.context();
        const auto page = base_page(context);
        if (!page || *page != controller.base_page_bytes() ||
            context.fingerprints().physical != controller.physical_fingerprint() ||
            context.fingerprints().effective != controller.effective_fingerprint())
            return std::unexpected{Failure::topology_mismatch};
        if (!std::ranges::equal(present_cpus(context), controller.present_cpus()))
            return std::unexpected{Failure::present_cpu_mismatch};
        const auto declared = input.request().workers();
        if (declared.size() != input.workers().size()) return std::unexpected{Failure::invalid_input};
        std::vector<core::LinuxCpuId> workers;
        workers.reserve(declared.size());
        for (std::size_t index{}; index < declared.size(); ++index) {
            if (&input.workers()[index].request() != &declared[index]) return std::unexpected{Failure::invalid_input};
            const auto cpu = declared[index].cpu().linux_cpu();
            if (std::ranges::find(workers, cpu) != workers.end())
                return std::unexpected{Failure::worker_order_invalid};
            if (!std::ranges::binary_search(controller.present_cpus(), cpu))
                return std::unexpected{Failure::worker_membership_invalid};
            workers.push_back(cpu);
        }
        const auto& monitoring = input.request().monitoring_inputs();
        const auto& measurement = input.execution_inputs().measurement();
        if (!monitoring || !matches_policy(*monitoring, policy) ||
            measurement.repetition_maximum_blocks() == 0U || measurement.observation_maximum_duration().value() == 0U)
            return std::unexpected{Failure::pairwise_policy_mismatch};
        const bool asynchronous = policy.disposition(monitoring::MonitoringChannel::context_switches) != monitoring::MonitoringChannelDisposition::disabled ||
            policy.disposition(monitoring::MonitoringChannel::temperature) != monitoring::MonitoringChannelDisposition::disabled ||
            policy.disposition(monitoring::MonitoringChannel::frequency) != monitoring::MonitoringChannelDisposition::disabled;
        if (!single_service_matches(input, service, asynchronous, workers, controller.present_cpus()))
            return std::unexpected{Failure::pairwise_policy_mismatch};
        std::optional<core::LinuxCpuId> service_cpu;
        for (const auto& assignment : service.assignments())
            if (assignment.role == ServiceRole::measurement_monitor) service_cpu = assignment.linux_cpu;
        const auto interval = policy.during_interval() ? std::optional{policy.during_interval()->value()} : std::nullopt;
        return PairwiseMonitoringHostCapacityAdmission{std::move(controller), declared.size(),
            measurement.repetition_maximum_blocks(), measurement.observation_maximum_duration().value(),
            interval, std::move(workers), service_cpu, policy, capacity_identity, profile_identity};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::matches_single(
    const PairwiseMonitoringHostCapacityAdmission& admission,
    const AdmittedPlacementRequest& input, const monitoring::MonitoringExecutionPolicy& policy,
    const ServicePlacementPlan& service) -> bool {
    const auto& authority = admission.authority();
    if (!single_eligible(input) || !authority.authentically_bound() ||
        admission.capacity_contract_identity() != expected_capacity_identity() ||
        admission.finalized_error_profile_identity() != expected_profile_identity())
        return false;
    const auto source = provenance_for(input.request().provenance());
    const auto& context = input.context();
    const auto page = base_page(context);
    if (!source || authority.provenance() != *source || !page ||
        *page != authority.base_page_bytes() ||
        context.fingerprints().physical != authority.physical_fingerprint() ||
        context.fingerprints().effective != authority.effective_fingerprint() ||
        !std::ranges::equal(context.topology().machine().present_cpus, authority.present_cpus()))
        return false;
    const auto declared = input.request().workers();
    const auto workers = admission.worker_cpus();
    if (declared.size() != input.workers().size() || workers.size() != declared.size() ||
        admission.worker_count() != declared.size())
        return false;
    for (std::size_t index{}; index < declared.size(); ++index) {
        const auto cpu = declared[index].cpu().linux_cpu();
        if (&input.workers()[index].request() != &declared[index] || cpu != workers[index] ||
            std::ranges::find(workers.first(index), cpu) != workers.first(index).end() ||
            !std::ranges::binary_search(authority.present_cpus(), cpu))
            return false;
    }
    const auto& monitoring = input.request().monitoring_inputs();
    const auto& measurement = input.execution_inputs().measurement();
    const auto interval = policy.during_interval()
        ? std::optional{policy.during_interval()->value()} : std::nullopt;
    if (!monitoring || !matches_policy(*monitoring, policy) || policy != admission.monitoring_policy() ||
        !measurement.repetition_maximum_blocks() ||
        measurement.repetition_maximum_blocks() != admission.maximum_blocks() ||
        !measurement.observation_maximum_duration().value() ||
        measurement.observation_maximum_duration().value() != admission.maximum_observation_nanoseconds() ||
        interval != admission.interval_nanoseconds())
        return false;
    const bool asynchronous = policy.disposition(monitoring::MonitoringChannel::context_switches) != monitoring::MonitoringChannelDisposition::disabled ||
        policy.disposition(monitoring::MonitoringChannel::temperature) != monitoring::MonitoringChannelDisposition::disabled ||
        policy.disposition(monitoring::MonitoringChannel::frequency) != monitoring::MonitoringChannelDisposition::disabled;
    if (!single_service_matches(input, service, asynchronous, workers, authority.present_cpus()))
        return false;
    std::optional<core::LinuxCpuId> service_cpu;
    for (const auto& assignment : service.assignments())
        if (assignment.role == ServiceRole::measurement_monitor) service_cpu = assignment.linux_cpu;
    return service_cpu == admission.service_cpu();
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::provenance_for(
    const PlacementProvenance& provenance)
    -> std::optional<platform_linux::detail::LinuxMonitoringCapacityProvenance> {
    if (const auto* scenario = std::get_if<ScenarioPlacementProvenance>(&provenance)) {
        return LowerAccess::scenario_provenance(
            scenario->scenario_digest().digest(),
            scenario->resolved_scenario_plan_digest().digest());
    }
    const auto& host = std::get<HostQualificationPlanPlacementProvenance>(provenance);
    return LowerAccess::host_plan_provenance(
        host.environment_digest().digest(),
        host.host_qualification_plan_digest().digest());
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::expected_capacity_identity()
    -> core::Sha256Digest {
    return core::sha256(pairwise_monitoring_capacity_contract_identity);
}
auto PairwiseMonitoringHostCapacityAdmissionAccess::expected_profile_identity()
    -> core::Sha256Digest {
    return core::sha256(pairwise_finalized_error_profile_identity);
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::mint(
    const AdmittedPairwiseProductionInputs& inputs, PairwiseVariant variant) noexcept
    -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    const auto& admitted = variant == PairwiseVariant::first
                               ? inputs.pairwise_request().first()
                               : inputs.pairwise_request().second();
    const auto& context = admitted.context();
    try {
        auto page = base_page(context);
        auto source = provenance_for(admitted.request().provenance());
        if (!page || !source)
            return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
                platform_linux::detail::LinuxMonitoringCapacityFailureKind::invalid_host_facts}};
        auto facts = LowerAccess::host_facts(*page, present_cpus(context),
                                             context.fingerprints().physical,
                                             context.fingerprints().effective,
                                             std::move(*source));
        return LowerAccess::authenticate(std::move(facts));
    } catch (const std::bad_alloc&) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::allocation}};
    } catch (const std::exception&) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::contained_exception}};
    } catch (...) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::contained_unknown_failure}};
    }
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::mint_with(
    const AdmittedPairwiseProductionInputs& inputs, PairwiseVariant variant,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations operations)
    noexcept
    -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    const auto& admitted = variant == PairwiseVariant::first
                               ? inputs.pairwise_request().first()
                               : inputs.pairwise_request().second();
    const auto& context = admitted.context();
    try {
        auto page = base_page(context);
        auto source = provenance_for(admitted.request().provenance());
        if (!page || !source)
            return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
                platform_linux::detail::LinuxMonitoringCapacityFailureKind::invalid_host_facts}};
        auto facts = LowerAccess::host_facts(*page, present_cpus(context),
                                             context.fingerprints().physical,
                                             context.fingerprints().effective,
                                             std::move(*source));
        return LowerAccess::authenticate_with(std::move(facts), operations);
    } catch (const std::bad_alloc&) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::allocation}};
    } catch (const std::exception&) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::contained_exception}};
    } catch (...) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::contained_unknown_failure}};
    }
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::mint_request_with(
    const RunnerContext& context, const ipc::PlacementRequest& request,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations operations)
    noexcept
    -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    try {
        auto page = base_page(context);
        auto source = provenance_for(request.provenance());
        if (!page || !source)
            return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
                platform_linux::detail::LinuxMonitoringCapacityFailureKind::invalid_host_facts}};
        auto facts = LowerAccess::host_facts(*page, present_cpus(context),
                                             context.fingerprints().physical,
                                             context.fingerprints().effective,
                                             std::move(*source));
        return LowerAccess::authenticate_with(std::move(facts), operations);
    } catch (const std::bad_alloc&) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::allocation}};
    } catch (const std::exception&) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::contained_exception}};
    } catch (...) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::contained_unknown_failure}};
    }
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::mint_request(
    const RunnerContext& context, const ipc::PlacementRequest& request) noexcept
    -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    try {
        auto page = base_page(context);
        auto source = provenance_for(request.provenance());
        if (!page || !source)
            return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
                platform_linux::detail::LinuxMonitoringCapacityFailureKind::invalid_host_facts}};
        auto facts = LowerAccess::host_facts(*page, present_cpus(context),
                                             context.fingerprints().physical,
                                             context.fingerprints().effective,
                                             std::move(*source));
        return LowerAccess::authenticate(std::move(facts));
    } catch (const std::bad_alloc&) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::allocation}};
    } catch (const std::exception&) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::contained_exception}};
    } catch (...) {
        return std::unexpected{platform_linux::detail::LinuxMonitoringCapacityFailure{
            platform_linux::detail::LinuxMonitoringCapacityFailureKind::contained_unknown_failure}};
    }
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::binding_seal(
    const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority& authority)
    noexcept -> platform_linux::detail::LinuxMonitoringCapacityLocalBindingSeal {
    return LowerAccess::binding_seal(authority);
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::project(
    const AdmittedPairwiseProductionInputs& inputs,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority controller,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority runner,
    const core::Sha256Digest& capacity_identity,
    const core::Sha256Digest& profile_identity) noexcept
    -> std::expected<PairwiseMonitoringHostCapacityAdmission, Failure> {
    return project_with(inputs, std::move(controller), std::move(runner),
                        capacity_identity, profile_identity, {});
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::project_with(
    const AdmittedPairwiseProductionInputs& inputs,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority controller,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority runner,
    const core::Sha256Digest& capacity_identity,
    const core::Sha256Digest& profile_identity,
    ValidationOperations validation) noexcept
    -> std::expected<PairwiseMonitoringHostCapacityAdmission, Failure> {
    try {
        if (!controller.authentically_bound())
            return std::unexpected{Failure::controller_unbound};
        if (!runner.authentically_bound())
            return std::unexpected{Failure::runner_unbound};
        if (!controller.stable_equals(runner))
            return std::unexpected{Failure::lower_authority_mismatch};
        return project_after_lower_validation(inputs, std::move(controller), capacity_identity,
                                              profile_identity, validation);
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::binding_matches(
    const platform_linux::detail::LinuxMonitoringCapacityLocalBindingSeal& seal,
    const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority& authority)
    noexcept -> bool {
    return LowerAccess::binding_matches(seal, authority);
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::statistics_match(
    const AdmittedPairwiseProductionInputs& inputs,
    const StatisticsComparisonPolicySource& source) noexcept -> bool {
    auto statistics_policy = StatisticsProductionPolicy::create(
        source, inputs.pairwise_request().first().execution_inputs().measurement());
    return statistics_policy && inputs.statistics_source_ == source &&
           inputs.statistics_policy_ == *statistics_policy;
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::provenance_matches(
    const PairwiseMonitoringHostCapacityAdmission& admission,
    const AdmittedPairwiseProductionInputs& inputs) noexcept -> bool {
    const auto left = provenance_for(
        inputs.pairwise_request().first().request().provenance());
    const auto right = provenance_for(
        inputs.pairwise_request().second().request().provenance());
    return left && right && *left == *right &&
           admission.authority().provenance() == *left;
}

auto PairwiseMonitoringHostCapacityAdmissionAccess::project_after_lower_validation(
    const AdmittedPairwiseProductionInputs& inputs,
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority authority,
    const core::Sha256Digest& capacity_identity,
    const core::Sha256Digest& profile_identity,
    ValidationOperations validation) noexcept
    -> std::expected<PairwiseMonitoringHostCapacityAdmission, Failure> {
    try {
        const auto accepted = [&](ValidationPoint point, bool value) noexcept {
            return validation.accept == nullptr
                       ? value
                       : validation.accept(validation.context, point, value);
        };
        if (capacity_identity != expected_capacity_identity())
            return std::unexpected{Failure::capacity_contract_identity_mismatch};
        if (profile_identity != expected_profile_identity())
            return std::unexpected{Failure::finalized_error_profile_identity_mismatch};

        const auto& pair = inputs.pairwise_request();
        const auto& first = pair.first();
        const auto& second = pair.second();
        if (first.context().fingerprints() != second.context().fingerprints() ||
            first.context().topology().machine().present_cpus !=
                second.context().topology().machine().present_cpus)
            return std::unexpected{Failure::topology_mismatch};
        auto expected_provenance = provenance_for(first.request().provenance());
        auto second_provenance = provenance_for(second.request().provenance());
        if (!expected_provenance || !second_provenance ||
            *expected_provenance != *second_provenance ||
            authority.provenance() != *expected_provenance)
            return std::unexpected{Failure::provenance_mismatch};
        const auto page = base_page(first.context());
        if (!page || *page != authority.base_page_bytes() ||
            first.context().fingerprints().physical != authority.physical_fingerprint() ||
            first.context().fingerprints().effective != authority.effective_fingerprint())
            return std::unexpected{Failure::topology_mismatch};
        if (present_cpus(first.context()) !=
            std::vector<core::LinuxCpuId>{authority.present_cpus().begin(),
                                          authority.present_cpus().end()})
            return std::unexpected{Failure::present_cpu_mismatch};

        const auto first_workers = first.request().workers();
        const auto second_workers = second.request().workers();
        if (!accepted(ValidationPoint::worker_count,
                      first_workers.size() == second_workers.size() &&
                          !first_workers.empty()))
            return std::unexpected{Failure::pairwise_policy_mismatch};
        std::set<core::LinuxCpuId> seen;
        std::optional<core::LinuxCpuId> prior;
        for (std::size_t index{}; index < first_workers.size(); ++index) {
            const auto left = first_workers[index].cpu().linux_cpu();
            const auto right = second_workers[index].cpu().linux_cpu();
            if (!accepted(ValidationPoint::worker_pairing, left == right))
                return std::unexpected{Failure::pairwise_policy_mismatch};
            if (!accepted(ValidationPoint::worker_order,
                          (!prior || left.value() > prior->value()) &&
                              seen.insert(left).second))
                return std::unexpected{Failure::worker_order_invalid};
            if (!accepted(ValidationPoint::worker_membership,
                          std::ranges::find(authority.present_cpus(), left) !=
                              authority.present_cpus().end()))
                return std::unexpected{Failure::worker_membership_invalid};
            prior = left;
        }
        const auto& first_measurement = first.execution_inputs().measurement();
        const auto& second_measurement = second.execution_inputs().measurement();
        const auto& policy = inputs.monitoring_policy();
        const auto& first_monitoring = first.request().monitoring_inputs();
        const auto& second_monitoring = second.request().monitoring_inputs();
        const bool intervals_equal =
            first_monitoring && second_monitoring &&
            first_monitoring->during_interval() == second_monitoring->during_interval() &&
            first_monitoring->during_interval() == policy.during_interval();
        const bool policies_equal =
            first_monitoring && second_monitoring &&
            *first_monitoring == *second_monitoring &&
            matches_policy(*first_monitoring, policy) &&
            matches_policy(*second_monitoring, policy);
        if (!accepted(ValidationPoint::maximum_blocks,
                      first_measurement.repetition_maximum_blocks() ==
                          second_measurement.repetition_maximum_blocks()) ||
            !accepted(ValidationPoint::observation_duration,
                      first_measurement.observation_maximum_duration() ==
                          second_measurement.observation_maximum_duration()) ||
            !accepted(ValidationPoint::interval, intervals_equal) ||
            !accepted(ValidationPoint::normalized_policy, policies_equal) ||
            !(first_measurement == second_measurement))
            return std::unexpected{Failure::pairwise_policy_mismatch};
        const auto interval = policy.during_interval()
                                  ? std::optional{policy.during_interval()->value()}
                                  : std::nullopt;
        std::vector<core::LinuxCpuId> worker_cpus;
        worker_cpus.reserve(first_workers.size());
        for (const auto& worker : first_workers)
            worker_cpus.push_back(worker.cpu().linux_cpu());
        std::optional<core::LinuxCpuId> service_cpu;
        bool duplicate_service{};
        for (const auto& assignment : inputs.service_placement().assignments()) {
            if (assignment.role != ServiceRole::measurement_monitor)
                continue;
            if (service_cpu)
                duplicate_service = true;
            else
                service_cpu = assignment.linux_cpu;
        }
        const bool service_valid = !duplicate_service &&
            service_cpu.has_value() == inputs.measurement_monitor_required() &&
            (!service_cpu ||
             std::ranges::find(worker_cpus, *service_cpu) == worker_cpus.end()) &&
            (!service_cpu ||
             std::ranges::find(authority.present_cpus(), *service_cpu) !=
                 authority.present_cpus().end());
        if (!accepted(ValidationPoint::service_assignment, service_valid))
            return std::unexpected{Failure::pairwise_policy_mismatch};
        return PairwiseMonitoringHostCapacityAdmission{
            std::move(authority), first_workers.size(),
            first_measurement.repetition_maximum_blocks(),
            first_measurement.observation_maximum_duration().value(), interval,
            std::move(worker_cpus), service_cpu, policy, capacity_identity,
            profile_identity};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

} // namespace epycbench::runtime::detail
