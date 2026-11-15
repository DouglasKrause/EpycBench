#include "results/expected_pair_evidence_context.hpp"
#include "runtime/finalized_evidence_wire_component.hpp"
#include "core/json_ownership.hpp"
#include "runtime/pairwise_monitoring_host_capacity_admission.hpp"

#include <limits>
#include <algorithm>
#include <bit>
#include <new>

namespace epycbench::results::block_evidence {
namespace wire = runtime::wire_v1;
namespace detail = wire::detail;
namespace {
struct Sum final {
    std::size_t value{};
    bool valid{true};
    void add(std::size_t count, std::size_t size = 1U) noexcept {
        const auto product = detail::multiply_component_stock(count, size);
        if (!product) { valid = false; return; }
        const auto total = detail::add_wire_json_count(value, *product);
        if (!total) valid = false; else value = *total;
    }
};
auto failure(ErrorCode code, std::size_t limit = 0, std::size_t observed = 0) noexcept {
    return std::unexpected{Error{code, Phase::context, limit, observed}};
}
// Fixed call-chain stock: provenance preimage, SHA state/tail/result/schedule,
// compress round locals, loop/length counters, views and digest return cells.
struct MonitoringProvenanceScratch final {
    std::array<std::byte, 4U + std::string_view{"epycbench.linux-monitoring-capacity-provenance/host-plan/v1"}.size() + 64U> preimage;
    std::array<std::uint32_t, 8> state;
    std::array<std::byte, 128> tail;
    std::array<std::byte, 32> result;
    std::array<std::uint32_t, 64> schedule;
    std::array<std::uint32_t, 16> round;
    std::array<std::size_t, 12> indices;
    std::uint64_t bits;
    std::array<std::string_view, 2> views;
    std::array<core::Sha256Digest, 3> returns;
};
constexpr auto scenario_profile = std::string_view{"epycbench.scenario/v0.1"};
}
struct ExpectedPairContext::State final {
    State(const journal::RunId& id, runtime::PairwiseRunProvenance p,
          const runtime::AdmittedPairwiseProductionRequest& pair,
          const wire::CalculatedEvidenceCapacity& bound, ContextUsage used)
        : run{id}, provenance{std::move(p)}, first{pair.first().execution_inputs().kernel()},
          second{pair.second().execution_inputs().kernel()}, capacity{bound}, usage{used} {}
    journal::RunId run;
    runtime::PairwiseRunProvenance provenance;
    runtime::AdmittedKernelExecutionInputs first, second;
    wire::CalculatedEvidenceCapacity capacity;
    std::vector<core::LinuxCpuId> cpus;
    std::vector<core::WorkerId> workers;
    std::vector<runtime::ipc::PlacementCpuAssignment> first_assignments, second_assignments;
    std::vector<ExpectedMemoryCell> first_memory, second_memory;
    ContextUsage usage;
    std::optional<runtime::ServiceThreadAssignment> control;
    std::optional<ExpectedMonitoringPreparation> monitoring;
    std::optional<ExpectedCompletePreparation> preparation;
};
ExpectedPairContext::ExpectedPairContext(std::shared_ptr<const State> state) noexcept : state_{std::move(state)} {}
ExpectedPairContext::ExpectedPairContext(ExpectedPairContext&&) noexcept = default;
auto ExpectedPairContext::operator=(ExpectedPairContext&&) noexcept -> ExpectedPairContext& = default;
ExpectedPairContext::~ExpectedPairContext() = default;
auto ExpectedPairContext::run() const noexcept -> const journal::RunId& { return state_->run; }
auto ExpectedPairContext::provenance() const noexcept -> const runtime::PairwiseRunProvenance& { return state_->provenance; }
auto ExpectedPairContext::cpus() const noexcept -> std::span<const core::LinuxCpuId> { return state_->cpus; }
auto ExpectedPairContext::workers() const noexcept -> std::span<const core::WorkerId> { return state_->workers; }
auto ExpectedPairContext::kernel(bool second) const noexcept -> const runtime::AdmittedKernelExecutionInputs& { return second ? state_->second : state_->first; }
auto ExpectedPairContext::assignments(bool second) const noexcept -> std::span<const runtime::ipc::PlacementCpuAssignment> { return second ? state_->second_assignments : state_->first_assignments; }
auto ExpectedPairContext::memory_requests(bool second) const noexcept -> std::span<const ExpectedMemoryCell> { return second ? state_->second_memory : state_->first_memory; }
auto ExpectedPairContext::capacity() const noexcept -> const wire::CalculatedEvidenceCapacity& { return state_->capacity; }
auto ExpectedPairContext::usage() const noexcept -> ContextUsage { return state_->usage; }

auto ExpectedPairContext::exactly_matches(const ExpectedPairContext& other) const noexcept -> bool {
    if (!state_ || !other.state_) return false;
    const auto& a = *state_;
    const auto& b = *other.state_;
    const auto& p = a.provenance;
    const auto& q = b.provenance;
    if (a.run.text() != b.run.text() || p.experiment_id != q.experiment_id ||
        p.scenario_profile != q.scenario_profile || p.scenario_digest != q.scenario_digest ||
        p.resolved_plan_digest != q.resolved_plan_digest ||
        p.workload_id != q.workload_id || p.first_variant_id != q.first_variant_id ||
        p.second_variant_id != q.second_variant_id || a.first != b.first ||
        a.second != b.second || a.capacity != b.capacity || a.cpus != b.cpus ||
        a.workers != b.workers || a.first_assignments != b.first_assignments ||
        a.second_assignments != b.second_assignments || a.control != b.control ||
        a.monitoring.has_value() != b.monitoring.has_value() ||
        a.preparation.has_value() != b.preparation.has_value()) return false;
    const auto memory = [](const auto& first, const auto& second) noexcept {
        return std::ranges::equal(first, second, [](const ExpectedMemoryCell& x, const ExpectedMemoryCell& y) {
            return x.request == y.request && x.resolved_nodes == y.resolved_nodes;
        });
    };
    if (!memory(a.first_memory, b.first_memory) || !memory(a.second_memory, b.second_memory)) return false;
    if (a.monitoring && (a.monitoring->policy != b.monitoring->policy ||
        a.monitoring->assignment != b.monitoring->assignment ||
        a.monitoring->physical != b.monitoring->physical ||
        a.monitoring->effective != b.monitoring->effective ||
        a.monitoring->scenario != b.monitoring->scenario)) return false;
    if (a.preparation) {
        const auto& x = *a.preparation;
        const auto& y = *b.preparation;
        const auto same = [](double first, double second) noexcept {
            return std::bit_cast<std::uint64_t>(first) == std::bit_cast<std::uint64_t>(second);
        };
        const auto& first_policy = x.statistics;
        const auto& second_policy = y.statistics;
        if (x.measurement != y.measurement || first_policy != second_policy ||
            x.first_request != y.first_request || x.second_request != y.second_request ||
            x.comparison_seed != y.comparison_seed ||
            !same(first_policy.comparison_bootstrap_policy().confidence().value(), second_policy.comparison_bootstrap_policy().confidence().value()) ||
            !same(first_policy.stopping_bootstrap_policy().confidence().value(), second_policy.stopping_bootstrap_policy().confidence().value()) ||
            !same(first_policy.effective_practical_effect_fraction(), second_policy.effective_practical_effect_fraction()) ||
            !same(first_policy.practical_effect_threshold().delta(), second_policy.practical_effect_threshold().delta()) ||
            !same(first_policy.relative_half_width_target().value(), second_policy.relative_half_width_target().value())) return false;
    }
    return true;
}

auto ExpectedPairContext::complete_preparation() const noexcept -> const ExpectedCompletePreparation* {
    return state_ && state_->preparation ? &*state_->preparation : nullptr;
}
auto ExpectedPairContext::monitoring_preparation() const noexcept -> const ExpectedMonitoringPreparation* {
    return state_ && state_->monitoring ? &*state_->monitoring : nullptr;
}
auto ExpectedPairContext::control_assignment() const noexcept -> const runtime::ServiceThreadAssignment* {
    return state_ && state_->control ? &*state_->control : nullptr;
}
auto ExpectedPairContext::prepare(const journal::RunId& run,
    const scenario::ScenarioDigestedPlan& plan, const runtime::AdmittedPairwiseProductionInputs& inputs,
    const wire::CalculatedEvidenceCapacity& capacity, ContextLimits limits)
    -> std::expected<ExpectedPairContext, Error> {
    const runtime::ServiceThreadAssignment* selected{};
    for (const auto& assignment : inputs.service_placement().assignments()) {
        if (assignment.role != runtime::ServiceRole::control_monitor &&
            assignment.role != runtime::ServiceRole::measurement_monitor) return failure(ErrorCode::identity);
        if (assignment.role == runtime::ServiceRole::control_monitor) {
            if (selected || assignment.stable_pu.value().empty() || assignment.stable_core.value().empty())
                return failure(ErrorCode::identity);
            selected = &assignment;
        }
    }
    if (!selected) return failure(ErrorCode::identity);
    return prepare_impl(run, plan, inputs.pairwise_request(), capacity, limits, selected, &inputs);
}
auto ExpectedPairContext::prepare(const journal::RunId& run,
    const scenario::ScenarioDigestedPlan& plan, const runtime::AdmittedPairwiseProductionRequest& pair,
    const wire::CalculatedEvidenceCapacity& capacity, ContextLimits limits)
    -> std::expected<ExpectedPairContext, Error> {
    return prepare_impl(run, plan, pair, capacity, limits, nullptr);
}
auto ExpectedPairContext::prepare_impl(const journal::RunId& run,
    const scenario::ScenarioDigestedPlan& plan, const runtime::AdmittedPairwiseProductionRequest& pair,
    const wire::CalculatedEvidenceCapacity& capacity, ContextLimits limits, const runtime::ServiceThreadAssignment* control, const runtime::AdmittedPairwiseProductionInputs* inputs)
    -> std::expected<ExpectedPairContext, Error> {
    try {
        if (limits.depth > 64U) return failure(ErrorCode::capacity, 64U, limits.depth);
        const auto& scenario = plan.canonical_scenario_json();
        const auto& resolved = plan.canonical_resolved_scenario_plan_json();
        if (scenario.size() > limits.scenario_bytes || resolved.size() > limits.resolved_plan_bytes)
            return failure(ErrorCode::capacity);
        const auto count = detail::count_component_wire_shape(scenario, limits.depth);
        if (!count) return failure(count.error().code == detail::WireJsonFailure::malformed
                                      ? ErrorCode::malformed : ErrorCode::capacity);
        if (count->nodes > limits.nodes || count->strings > limits.strings)
            return failure(ErrorCode::capacity);
        const auto& first = pair.first().request();
        const auto& second = pair.second().request();
        const auto first_prov = first.scenario_provenance();
        const auto second_prov = second.scenario_provenance();
        if (run.text().size() != 36U || !first_prov || !second_prov ||
            first_prov->get().scenario_digest() != plan.scenario_digest() ||
            second_prov->get().scenario_digest() != plan.scenario_digest() ||
            first_prov->get().resolved_scenario_plan_digest() != plan.resolved_scenario_plan_digest() ||
            second_prov->get().resolved_scenario_plan_digest() != plan.resolved_scenario_plan_digest() ||
            core::sha256(scenario) != plan.scenario_digest().digest() ||
            core::sha256(resolved) != plan.resolved_scenario_plan_digest().digest())
            return failure(ErrorCode::identity);
        if (inputs && sizeof(MonitoringProvenanceScratch) > limits.simultaneous_bytes)
            return failure(ErrorCode::capacity);
        const auto monitoring_provenance = inputs
            ? runtime::detail::PairwiseMonitoringHostCapacityAdmissionAccess::provenance_for(first.provenance())
            : std::nullopt;
        if (inputs && !monitoring_provenance) return failure(ErrorCode::identity);
        const auto worker_count = pair.first().workers().size();
        if (!worker_count || worker_count > limits.workers ||
            worker_count > std::numeric_limits<std::uint32_t>::max() ||
            second.workers().size() != worker_count)
            return failure(ErrorCode::identity);
        for (std::size_t i{}; i < worker_count; ++i)
            if (first.workers()[i].cpu().linux_cpu() != second.workers()[i].cpu().linux_cpu())
                return failure(ErrorCode::identity);
        if (pair.first().execution_inputs().kernel().index() != pair.second().execution_inputs().kernel().index())
            return failure(ErrorCode::identity);
        const runtime::ServiceThreadAssignment* measurement{};
        if (inputs) for (const auto& assignment : inputs->service_placement().assignments())
            if (assignment.role == runtime::ServiceRole::measurement_monitor) {
                if (measurement) return failure(ErrorCode::identity);
                measurement = &assignment;
            }
        Sum service_strings;
        if (control) {
            service_strings.add(control->stable_pu.value().size());
            service_strings.add(control->stable_core.value().size());
            if (!service_strings.valid) return failure(ErrorCode::capacity);
        }
        Sum monitoring_strings;
        if (measurement) {
            monitoring_strings.add(measurement->stable_pu.value().size());
            monitoring_strings.add(measurement->stable_core.value().size());
            if (!monitoring_strings.valid) return failure(ErrorCode::capacity);
        }
        service_strings.add(monitoring_strings.value);
        if (!service_strings.valid) return failure(ErrorCode::capacity);
        Sum identities{service_strings.value};
        identities.add(run.text().size()); identities.add(scenario_profile.size());
        identities.add(2U, 71U); // Exact sha256: plus 64 hex digits, each retained once.
        identities.add(first.experiment().workload_id().value().size());
        identities.add(2U, first.experiment().id().value().size());
        identities.add(std::string_view{":first"}.size() + std::string_view{":second"}.size());
        for (std::size_t i{}; i < worker_count; ++i) {
            for (const auto* assignment : {&first.workers()[i].cpu(), &second.workers()[i].cpu()}) {
                identities.add(assignment->stable_pu_id().value().size());
                identities.add(assignment->stable_core_id().value().size());
            }
        }
        Sum memory;
        memory.add(worker_count, 2U * sizeof(ExpectedMemoryCell));
        for (const auto* admitted : {&pair.first(), &pair.second()}) {
            for (const auto& worker : admitted->workers()) {
                const auto& region = worker.request().region();
                if (region.nodes().size() != worker.numa_nodes().size()) return failure(ErrorCode::identity);
                if (region.nodes().size() > std::vector<runtime::ipc::PlacementNumaTarget>{}.max_size() ||
                    region.nodes().size() > std::vector<core::NumaNodeId>{}.max_size()) return failure(ErrorCode::capacity);
                memory.add(region.nodes().size(), sizeof(runtime::ipc::PlacementNumaTarget) + sizeof(core::NumaNodeId));
                for (const auto& target : region.nodes()) identities.add(target.stable_numa_node_id().value().size());
            }
        }
        if (worker_count > std::vector<ExpectedMemoryCell>{}.max_size() || !memory.valid)
            return failure(ErrorCode::capacity);
        if (worker_count > std::vector<runtime::ipc::PlacementCpuAssignment>{}.max_size())
            return failure(ErrorCode::capacity);
        Sum retained;
        retained.add(sizeof(ExpectedPairContext)); retained.add(sizeof(State));
        retained.add(identities.value); retained.add(memory.value);
        retained.add(worker_count, sizeof(core::LinuxCpuId) + sizeof(core::WorkerId));
        retained.add(worker_count, 2U * sizeof(runtime::ipc::PlacementCpuAssignment));
        Sum elements;
        const auto& p = capacity.prerequisite.parameters;
        auto stock = [&](const auto& values) {
            using Value = typename std::remove_cvref_t<decltype(values)>::value_type;
            elements.add(values.size()); retained.add(values.size(), sizeof(Value));
        };
        stock(p.present_cpu_ids); stock(p.domain_cpu_ids);
        for (const auto& domain : p.domain_cpu_ids) stock(domain);
        stock(p.first_total_pages); stock(p.second_total_pages);
        stock(p.first_target_counts); stock(p.second_target_counts);
        if (!identities.valid || !retained.valid || !elements.valid ||
            identities.value > limits.identity_bytes || retained.value > limits.retained_bytes ||
            elements.value > limits.capacity_elements)
            return failure(ErrorCode::capacity);
        const auto dom = detail::component_dom_stock(count->nodes, count->strings);
        const auto parser = detail::component_parser_stock(count->bytes, count->depth);
        const auto renderer = detail::component_renderer_stock(count->depth, count->members, count->bytes);
        if (!dom || !parser || !renderer) return failure(ErrorCode::capacity);
        Sum peak{retained.value}; peak.add(2U, sizeof(ExpectedMemoryCell)); peak.add(*dom); peak.add(*parser); peak.add(*renderer);
        if (control) { peak.add(sizeof(runtime::ServiceThreadAssignment)); peak.add(service_strings.value); }
        if (inputs) {
            peak.add(sizeof(ExpectedMonitoringPreparation));
            peak.value = std::max(peak.value, sizeof(MonitoringProvenanceScratch));
        }
        if (!peak.valid || peak.value > limits.simultaneous_bytes)
            return failure(ErrorCode::capacity, limits.simultaneous_bytes, peak.value);
        auto decoded = detail::parse_wire_json(scenario, count->depth);
        if (!decoded) return failure(ErrorCode::malformed);
        const auto& value = decoded->value;
        const auto index = first.experiment().index();
        const auto experiments = value.find("experiments");
        if (experiments == value.end() || !experiments->is_array() || index >= experiments->size())
            return failure(ErrorCode::identity);
        const auto& experiment = (*experiments)[static_cast<std::size_t>(index)];
        const auto id = experiment.find("id");
        const auto workload = experiment.find("workload");
        if (id == experiment.end() || workload == experiment.end() ||
            !core::detail::json_string_equal(*id, first.experiment().id().value()) ||
            !core::detail::json_string_equal(*workload, first.experiment().workload_id().value()))
            return failure(ErrorCode::identity);
        runtime::PairwiseRunProvenance provenance{
            core::ExperimentId{index}, std::string{scenario_profile}, plan.scenario_digest().text(),
            plan.resolved_scenario_plan_digest().text(), first.experiment().workload_id().to_string(),
            first.experiment().id().to_string() + ":first", first.experiment().id().to_string() + ":second"};
        auto state = std::make_shared<State>(run, std::move(provenance), pair, capacity,
            ContextUsage{retained.value, peak.value, identities.value, elements.value});
        if (control) state->control.emplace(*control);
        if (inputs) {
            state->monitoring.emplace(ExpectedMonitoringPreparation{inputs->monitoring_policy(), {},
                first.topology().physical_fingerprint(), first.topology().effective_fingerprint(),
                monitoring_provenance->authority_digest()});
            if (measurement) state->monitoring->assignment.emplace(*measurement);
        }
        state->cpus.reserve(worker_count); state->workers.reserve(worker_count);
        if (inputs) state->preparation.emplace(pair.first().execution_inputs().measurement(), inputs->statistics_policy(),
            pair.first().request_id().value(), pair.second().request_id().value(), pair.resolved_comparison_order_seed());
        state->first_assignments.reserve(worker_count);
        state->second_assignments.reserve(worker_count);
        state->first_memory.reserve(worker_count); state->second_memory.reserve(worker_count);
        for (std::size_t i{}; i < worker_count; ++i) {
            // Each temporary cell owns its copies until noexcept transfer into the
            // array; a later nested copy failure leaves completed cells in State.
            const auto& a = pair.first().workers()[i];
            const auto& b = pair.second().workers()[i];
            state->first_memory.push_back({a.request().region(), {a.numa_nodes().begin(), a.numa_nodes().end()}});
            state->second_memory.push_back({b.request().region(), {b.numa_nodes().begin(), b.numa_nodes().end()}});
            state->first_assignments.push_back(first.workers()[i].cpu());
            state->second_assignments.push_back(second.workers()[i].cpu());
            state->cpus.push_back(first.workers()[i].cpu().linux_cpu());
            state->workers.emplace_back(static_cast<std::uint32_t>(i));
        }
        return ExpectedPairContext{std::move(state)};
    } catch (const std::bad_alloc&) { return failure(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return failure(ErrorCode::capacity); }
      catch (const nlohmann::json::exception&) { return failure(ErrorCode::malformed); }
}
PairFactsLease::PairFactsLease(ExpectedPairContext&& value) noexcept : value_{std::move(value)} {}
PairFactsLease::PairFactsLease(const ExpectedPairContext& value) noexcept
    : value_{ExpectedPairContext{value.state_}} {}
PairFactsLease::PairFactsLease(std::shared_ptr<const DecodedPairFacts> value) noexcept : value_{std::move(value)} {}
auto PairFactsLease::origin() const noexcept -> Origin {
    return value_.index()==0U?Origin::actual_context:Origin::decoded_evidence;
}
auto PairFactsLease::actual_context() const noexcept -> const ExpectedPairContext* {
    const auto* actual=std::get_if<ExpectedPairContext>(&value_);
    return actual && actual->available()?actual:nullptr;
}
auto PairFactsLease::available() const noexcept -> bool {
    if(const auto* actual=std::get_if<ExpectedPairContext>(&value_))return actual->available();
    return bool(std::get<1>(value_));
}
auto PairFactsLease::same_owner(const PairFactsLease& other) const noexcept -> bool {
    if(!available() || !other.available() || origin()!=other.origin())return false;
    if(const auto* actual=actual_context())return actual->state_==other.actual_context()->state_;
    return std::get<1>(value_)==std::get<1>(other.value_);
}
auto PairFactsLease::retain() const noexcept -> PairFactsLease {
    if(const auto* actual=std::get_if<ExpectedPairContext>(&value_))return PairFactsLease{*actual};
    return PairFactsLease{std::get<1>(value_)};
}
auto PairFactsLease::run() const noexcept -> const journal::RunId& {
    if(const auto* actual=actual_context())return actual->run();
    return std::get<1>(value_)->run;
}
auto PairFactsLease::provenance() const noexcept -> const runtime::PairwiseRunProvenance& {
    if(const auto* actual=actual_context())return actual->provenance();
    return std::get<1>(value_)->provenance;
}
auto PairFactsLease::capacity() const noexcept -> const runtime::wire_v1::CalculatedEvidenceCapacity& {
    if(const auto* actual=actual_context())return actual->capacity();
    return std::get<1>(value_)->capacity;
}
auto PairFactsLease::cpus() const noexcept -> std::span<const core::LinuxCpuId> {
    if(const auto* actual=actual_context())return actual->cpus();
    return std::get<1>(value_)->cpus;
}
auto PairFactsLease::workers() const noexcept -> std::span<const core::WorkerId> {
    if(const auto* actual=actual_context())return actual->workers();
    return std::get<1>(value_)->workers;
}
auto PairFactsLease::kernel(bool second) const noexcept -> const runtime::ipc::PlacementKernelExecutionInputs& {
    if(const auto* actual=actual_context())return actual->kernel(second);
    const auto& facts=*std::get<1>(value_);
    return second?facts.second:facts.first;
}
auto PairFactsLease::assignments(bool second) const noexcept -> std::span<const runtime::ipc::PlacementCpuAssignment> {
    if(const auto* actual=actual_context())return actual->assignments(second);
    const auto& facts=*std::get<1>(value_);
    return second?facts.second_assignments:facts.first_assignments;
}
auto PairFactsLease::memory_requests(bool second) const noexcept -> std::span<const ExpectedMemoryCell> {
    if(const auto* actual=actual_context())return actual->memory_requests(second);
    const auto& facts=*std::get<1>(value_);
    return second?facts.second_memory:facts.first_memory;
}
auto PairFactsLease::control_assignment() const noexcept -> const runtime::ServiceThreadAssignment* {
    if(const auto* actual=actual_context())return actual->control_assignment();
    if(!available())return nullptr;
    const auto& field=std::get<1>(value_)->control;
    return field?&*field:nullptr;
}
auto PairFactsLease::monitoring_preparation() const noexcept -> const ExpectedMonitoringPreparation* {
    if(const auto* actual=actual_context())return actual->monitoring_preparation();
    if(!available())return nullptr;
    const auto& field=std::get<1>(value_)->monitoring;
    return field?&*field:nullptr;
}
auto PairFactsLease::complete_preparation() const noexcept -> const ExpectedCompletePreparation* {
    if(const auto* actual=actual_context())return actual->complete_preparation();
    if(!available())return nullptr;
    const auto& field=std::get<1>(value_)->preparation;
    return field?&*field:nullptr;
}
auto PairFactsLease::usage() const noexcept -> ContextUsage {
    if(!available())return {};
    if(const auto* actual=actual_context()) {
        auto result=actual->usage();
        constexpr auto extra=sizeof(PairFactsLease)-sizeof(ExpectedPairContext);
        result.retained_bytes=result.retained_bytes>SIZE_MAX-extra?SIZE_MAX:result.retained_bytes+extra;
        result.simultaneous_bytes=result.simultaneous_bytes>SIZE_MAX-extra?SIZE_MAX:result.simultaneous_bytes+extra;
        return result;
    }
    return std::get<1>(value_)->usage;
}
auto PairFactsLease::view() const noexcept -> PairFactsView {
    return {run(),provenance(),kernel(false),kernel(true),capacity(),cpus(),workers(),
        assignments(false),assignments(true),memory_requests(false),memory_requests(true),
        control_assignment(),monitoring_preparation(),complete_preparation(),usage()};
}
} // namespace epycbench::results::block_evidence
