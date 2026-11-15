#include "runtime/finalized_evidence_wire_source_projection.hpp"

#include "epycbench/runtime/placement_provenance.hpp"
#include "monitoring/interruptible_raw_scheduler_kernel.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <variant>

namespace epycbench::runtime::wire_v1::detail {
namespace {
using Lower = platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority;
using Admission = runtime::detail::PairwiseMonitoringHostCapacityAdmission;

constexpr std::string_view registry_hex =
    "626c6355f54a5f7abea5a67cd6c8db95efd423540291145dc050c976c9038f48";
constexpr std::string_view source_manifest_hex =
    "2c5cf4461e02b8d8aa3c95a0b3918237bbb7d3af8c6a4c631008de08ea56c09b";

auto invalid(CapacityField field = CapacityField::input) -> CapacityError {
    return {CapacityErrorCode::invalid_input, field};
}
auto escaped(std::string_view value) -> std::uint64_t {
    const nlohmann::json string_value = std::string{value};
    const auto encoded = string_value.dump();
    return static_cast<std::uint64_t>(encoded.size() - 2U);
}
auto maximum_escaped(std::initializer_list<std::string> values) -> CapacityCount {
    std::uint64_t result{};
    for (const auto& value : values) result = std::max(result, escaped(value));
    return {result};
}
auto digest(std::string_view hex) -> std::optional<core::Sha256Digest> {
    auto parsed = core::Sha256Digest::parse(hex);
    if (!parsed) return std::nullopt;
    return *parsed;
}

struct ProjectionStorage final {
    std::vector<CpuId> present;
    std::vector<std::vector<CpuId>> domains;
    std::vector<std::span<const CpuId>> domain_spans;
    std::vector<PageCount> first_pages, second_pages;
    std::vector<TargetCount> first_targets, second_targets;
};

auto derive_domains(const RunnerContext&, std::span<const core::LinuxCpuId> present,
                    std::uint64_t workers,
                    ProjectionStorage& out) -> bool {
    const auto count = std::min<std::uint64_t>(workers, present.size());
    if (count == 0U) return false;
    out.domains.resize(static_cast<std::size_t>(count));
    for (std::size_t index{}; index < present.size(); ++index) {
        const auto domain = std::min(index, out.domains.size() - 1U);
        out.domains[domain].push_back(CpuId{present[index].value()});
    }
    for (const auto& domain : out.domains) out.domain_spans.emplace_back(domain);
    return !out.domains.empty();
}

auto derive_pages(const ipc::PlacementRequest& request,
                  std::vector<PageCount>& pages, std::vector<TargetCount>& targets) -> bool {
    for (const auto& worker : request.workers()) {
        const auto bytes = worker.region().bytes().value();
        const auto page = worker.region().page_size_bytes().value();
        if (page == 0U || bytes == 0U || bytes % page != 0U ||
            worker.region().nodes().empty()) return false;
        pages.push_back(PageCount{bytes / page});
        targets.push_back(TargetCount{worker.region().nodes().size()});
    }
    return true;
}

auto request_bytes(const ipc::PlacementRequest& first, const ipc::PlacementRequest& second,
                   std::uint64_t base_page) -> RequestKnownEscapedBytes {
    const auto& experiment = first.experiment();
    std::string scenario_digest, plan_digest, scenario_profile;
    if (const auto* scenario = std::get_if<ScenarioPlacementProvenance>(&first.provenance())) {
        scenario_profile = "epycbench.scenario/v0.1";
        scenario_digest = scenario->scenario_digest().text();
        plan_digest = scenario->resolved_scenario_plan_digest().text();
    } else {
        const auto& host = std::get<HostQualificationPlanPlacementProvenance>(first.provenance());
        scenario_profile = host.policy_identity().text();
        scenario_digest = host.environment_digest().text();
        plan_digest = host.host_qualification_plan_digest().text();
    }
    std::uint64_t stable_node{};
    for (const auto* request : {&first, &second})
        for (const auto& worker : request->workers())
            for (const auto& node : worker.region().nodes())
                stable_node = std::max(stable_node,
                    escaped(node.stable_numa_node_id().value()));
    // Monitoring implementation tokens are a closed registry. Frequency
    // driver/governor strings are bounded by the authenticated one-page read
    // contract; JSON escaping can expand each input byte by at most two.
    const auto dynamic_escaped = base_page > std::numeric_limits<std::uint64_t>::max() / 2U
        ? std::numeric_limits<std::uint64_t>::max() : base_page * 2U;
    return {
        {escaped(scenario_profile)}, {escaped(scenario_digest)}, {escaped(plan_digest)},
        {escaped(experiment.workload_id().to_string())},
        {escaped(experiment.id().to_string() + ":first")},
        {escaped(experiment.id().to_string() + ":second")},
        maximum_escaped({"linux-proc-task-stat", "linux-perf-context-switch-records",
                         "linux-hwmon-k10temp-tctl", "linux-cpufreq-cpuinfo-cur-freq",
                         "linux-cpufreq-scaling-cur-freq"}),
        {dynamic_escaped}, {dynamic_escaped}, {stable_node}};
}

auto project(const ipc::PlacementRequest& first, const ipc::PlacementRequest& second,
             const RunnerContext& context, const Lower& authority,
             const monitoring::MonitoringExecutionPolicy& policy,
             const core::Sha256Digest& request_sha,
             const core::Sha256Digest& finalized_error,
             const core::Sha256Digest& monitoring_capacity, LocalResourceLimits limits) noexcept
    -> std::expected<ProjectedCapacityContext, CapacityError> {
    try {
        if (!authority.authentically_bound() ||
            authority.physical_fingerprint() != context.fingerprints().physical ||
            authority.effective_fingerprint() != context.fingerprints().effective ||
            first.workers().size() != second.workers().size() || first.workers().empty())
            return std::unexpected{invalid()};
        const auto first_provenance =
            runtime::detail::PairwiseMonitoringHostCapacityAdmissionAccess::provenance_for(
                first.provenance());
        const auto second_provenance =
            runtime::detail::PairwiseMonitoringHostCapacityAdmissionAccess::provenance_for(
                second.provenance());
        if (!first_provenance || !second_provenance ||
            *first_provenance != *second_provenance ||
            authority.provenance() != *first_provenance)
            return std::unexpected{invalid()};
        ProjectionStorage storage;
        for (const auto cpu : authority.present_cpus())
            storage.present.push_back(CpuId{cpu.value()});
        if (!derive_domains(context, authority.present_cpus(), first.workers().size(), storage) ||
            !derive_pages(first, storage.first_pages,
                          storage.first_targets) ||
            !derive_pages(second, storage.second_pages,
                          storage.second_targets) || context.topology().machine().present_nodes.empty())
            return std::unexpected{invalid()};
        auto last = context.topology().machine().present_nodes.begin();
        for (auto node = last; node != context.topology().machine().present_nodes.end(); ++node)
            last = node;
        if ((*last).value() == std::numeric_limits<std::uint32_t>::max())
            return std::unexpected{invalid()};
        const auto active = [&](monitoring::MonitoringChannel channel) {
            return policy.disposition(channel) !=
                   monitoring::MonitoringChannelDisposition::disabled;
        };
        const auto page = active(monitoring::MonitoringChannel::page_faults);
        const auto context_channel = active(monitoring::MonitoringChannel::context_switches);
        const auto temperature = active(monitoring::MonitoringChannel::temperature);
        const auto frequency = active(monitoring::MonitoringChannel::frequency);
        const auto& measurement = first.execution_inputs().measurement();
        const auto interval = policy.during_interval();
        auto rounds = derive_monitoring_round_capacity(
            context_channel, temperature, frequency,
            measurement.observation_maximum_duration().value(),
            interval ? std::optional{interval->value()} : std::nullopt);
        if (!rounds) return std::unexpected{rounds.error()};
        const CapacityParameterCandidate candidate{
            WorkerCount{first.workers().size()},
            BlockCount{measurement.repetition_maximum_blocks()},
            WarmupTrialCount{measurement.warmup_maximum()},
            BasePageBytes{authority.base_page_bytes().value()},
            MaxCpuId{authority.kernel_maximum_cpu()},
            NumaMaxNode{static_cast<std::uint64_t>((*last).value()) + 1U},
            *rounds,
            {page, context_channel, temperature, frequency},
            request_bytes(first, second, authority.base_page_bytes().value()),
            storage.present, storage.domain_spans, storage.first_pages, storage.first_targets,
            storage.second_pages, storage.second_targets};
        auto normalized = normalize_capacity_parameters(candidate);
        if (!normalized) return std::unexpected{normalized.error()};
        auto capacity = calculate_evidence_capacity(*normalized, limits);
        if (!capacity) return std::unexpected{capacity.error()};
        const auto registry = digest(registry_hex);
        const auto source = digest(source_manifest_hex);
        if (!registry || !source) return std::unexpected{invalid()};
        return ProjectedCapacityContext{
            core::sha256(capacity_projection_context_profile), request_sha,
            core::sha256(wire_profile), core::sha256(capacity_profile),
            *registry, *source, finalized_error, monitoring_capacity, std::move(*capacity)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{CapacityError{CapacityErrorCode::allocation_failure,
                                             CapacityField::input}};
    } catch (...) {
        return std::unexpected{invalid()};
    }
}
} // namespace

auto derive_monitoring_round_capacity(
    bool context, bool temperature, bool frequency, std::uint64_t duration,
    std::optional<std::uint64_t> interval) noexcept
    -> std::expected<MonitoringRounds, CapacityError> {
    const auto asynchronous = context || temperature || frequency;
    if (!asynchronous) return MonitoringRounds{0U};
    if (context && !temperature && !frequency && !interval)
        return MonitoringRounds{1U};
    if (duration == 0U || !interval || *interval == 0U)
        return std::unexpected{invalid()};
    const auto period = temperature || frequency
        ? monitoring::detail::point_acquisition_period_nanoseconds(*interval)
        : *interval;
    const auto quotient = (duration - 1U) / period;
    if (quotient > std::numeric_limits<std::uint64_t>::max() - 2U)
        return std::unexpected{CapacityError{CapacityErrorCode::arithmetic_overflow,
                                             CapacityField::input}};
    return MonitoringRounds{2U + quotient};
}

auto project_controller_startup_capacity(
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    const RunnerContext& context, const Lower& authority, LocalResourceLimits limits) noexcept
    -> std::expected<ProjectedCapacityContext, CapacityError> {
    auto first_admitted = admit_placement_request(request.first_request_id(), request.first(), context);
    auto second_admitted = admit_placement_request(request.second_request_id(), request.second(), context);
    if (!first_admitted || !second_admitted) return std::unexpected{invalid()};
    auto inputs = AdmittedPairwiseProductionInputs::create(
        std::move(*first_admitted), std::move(*second_admitted),
        request.resolved_comparison_order_seed(), request.statistics_source());
    if (!inputs) return std::unexpected{invalid()};
    return project(request.first(), request.second(), context, authority,
                   inputs->monitoring_policy(), request.body_sha256(),
                   runtime::detail::PairwiseMonitoringHostCapacityAdmissionAccess::
                       expected_profile_identity(),
                   runtime::detail::PairwiseMonitoringHostCapacityAdmissionAccess::
                       expected_capacity_identity(), limits);
}

auto project_runner_startup_capacity(
                                     const runtime::detail::PairwiseHostCapacityRequestTranscript& transcript,
                                     const AdmittedPairwiseProductionInputs& inputs,
                                     const Admission& admission, LocalResourceLimits limits) noexcept
    -> std::expected<ProjectedCapacityContext, CapacityError> {
    const auto& pair = inputs.pairwise_request();
    if (pair.first().request_id() != transcript.first_request_id() ||
        pair.second().request_id() != transcript.second_request_id() ||
        pair.resolved_comparison_order_seed() != transcript.resolved_comparison_order_seed() ||
        pair.first().request() != transcript.first() || pair.second().request() != transcript.second())
        return std::unexpected{invalid()};
    return project(pair.first().request(), pair.second().request(), pair.first().context(),
                   admission.authority(), admission.monitoring_policy(),
                   transcript.body_sha256(), admission.finalized_error_profile_identity(),
                   admission.capacity_contract_identity(), limits);
}

auto validate_exact_projected_capacity_context(const ProjectedCapacityContext& expected,
                                               const ProjectedCapacityContext& observed) noexcept
    -> std::expected<void, CapacityError> {
    if (expected == observed) return {};
    return std::unexpected{CapacityError{CapacityErrorCode::mismatch,
                                         CapacityField::resource_plan}};
}

} // namespace epycbench::runtime::wire_v1::detail
