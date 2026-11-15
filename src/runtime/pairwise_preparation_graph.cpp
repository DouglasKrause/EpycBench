#include "runtime/pairwise_preparation_graph.hpp"
#include "runtime/pairwise_typed_production_binding_preparation.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace epycbench::runtime::detail {
static_assert(std::is_trivially_copyable_v<PreparationWorkerCell>);
static_assert(std::is_trivially_copyable_v<PreparationMemoryCell>);
static_assert(std::is_trivially_copyable_v<PreparationKernelCell>);
static_assert(std::is_trivially_copyable_v<PreparationTargetCell>);
static_assert(std::is_trivially_copyable_v<PreparationPageCountCell>);
static_assert(std::is_trivially_copyable_v<PreparationFrequencyCell>);
static_assert(std::is_trivially_copy_constructible_v<PreparationGraphPolicies>);
static_assert(std::is_trivially_move_constructible_v<PreparationGraphPolicies>);
static_assert(std::is_trivially_destructible_v<PreparationGraphPolicies>);
static_assert(std::is_trivially_copyable_v<PreparationMonitoringIdentity>);
namespace {
struct Stock final {
    std::size_t value{};
    bool valid{true};
    void add(std::size_t count, std::size_t size = 1U) noexcept {
        if (size && count > (std::numeric_limits<std::size_t>::max() - value) / size) valid = false;
        else value += count * size;
    }
};
struct ConstructionLocals final {
    const ScenarioPlacementProvenance* provenance_source{};
    std::array<std::string_view, 8> provenance;
    Stock strings, targets, pages, labels, retained, peak;
    std::array<std::array<char,71>,2> digest_text{};
    unsigned digest_byte{};
    std::size_t workers{}, rows{}, side{}, index{}, cursor{}, target{}, page{};
};
struct PreparationGraphStaging final {
    std::span<const WorkerReadyEvidence> workers;
    const PreparationGraphMemoryView* memory{};
    const MonitoringPreparationFactView* monitoring{};
    std::optional<MonitoringPreparationFrequencyView> frequency;
    std::array<PreparationSlice,4> slices;
    std::size_t index{}, members{};
    PreparationGraphFailure result{};
    std::shared_ptr<PairwisePreparationGraph::State> graph;
};
struct PreparationRequestComparisonStaging final {
    const PreparationTargetCell* target{};
    const memory::RegionNumaTarget* other{};
    std::size_t index{};
};
struct PreparationSourceCallerStaging final {
    const memory::ReadOnlyRegion* memory;
    std::size_t index;
    bool second, latency;
};
struct PreparationComparisonStaging final {
    const PreparationMemoryCell* memory;
    const PreparationWorkerCell* worker;
    const PreparationGraphPolicies* policy;
    const PreparationMonitoringIdentity* monitoring;
    std::size_t index;
    std::optional<MonitoringPreparationFrequencyView> first, second;
    std::array<std::optional<measurement::WarmupStabilityThreshold>,2> warmup_returns;
    std::array<std::optional<measurement::MaximumHarnessOverhead>,2> overhead_returns;
};
} // namespace
struct PairwisePreparationGraph::State final {
    MonitoringPreparationFactDomain domain;
    PreparationGraphStage stage{PreparationGraphStage::allocated};
    bool retired{}, failed{};
    PreparationGraphUsage usage;
    std::optional<PreparationGraphPolicies> policies;
    std::optional<PreparationMonitoringIdentity> monitoring;
    std::array<PreparationSlice, 8> provenance;
    std::optional<AdmittedMeasurementInputs> measurement;
    std::uint64_t experiment{}, first_request{}, second_request{};
    std::vector<PreparationWorkerCell> workers;
    std::vector<PreparationMemoryCell> memory;
    std::vector<PreparationKernelCell> kernels;
    std::vector<PreparationTargetCell> targets;
    std::vector<PreparationPageCountCell> pages;
    std::vector<PreparationFrequencyCell> frequency;
    std::vector<core::LinuxCpuId> cpus;
    std::vector<char> strings;
    PreparationGraphDimensions dimensions;
    std::size_t frequency_count{};
    explicit State(MonitoringPreparationFactDomain value) noexcept : domain{value} {}
    auto text(PreparationSlice value) const noexcept -> std::string_view {
        return {strings.data() + value.offset, value.size};
    }
    auto copy_text(std::size_t& cursor, std::string_view value) noexcept -> PreparationSlice {
        const PreparationSlice slice{cursor, value.size()};
        std::copy(value.begin(), value.end(), strings.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += value.size();
        return slice;
    }
};
// Empty max_size query headers coexist only in preflight, before State allocation.
// Their full-expression stock is dominated by the later charged State header.
static_assert(sizeof(PairwisePreparationGraph::State) >=
    sizeof(std::vector<PreparationWorkerCell>) + sizeof(std::vector<PreparationMemoryCell>) +
    sizeof(std::vector<PreparationKernelCell>) + sizeof(std::vector<PreparationTargetCell>) +
    sizeof(std::vector<PreparationPageCountCell>) + sizeof(std::vector<PreparationFrequencyCell>) +
    2U*sizeof(std::vector<core::LinuxCpuId>) + 2U*sizeof(std::vector<char>));
auto PairwisePreparationGraph::prepare(const PairwiseTypedPreOwnerPreparation& source,
    const AdmittedPairwiseProductionInputs& inputs, const PreparationGraphDimensions& dimensions,
    const PreparationGraphLimits& limits, MonitoringPreparationFactDomain domain)
    -> std::expected<PairwisePreparationGraph, PreparationGraphFailure> {
    const PreparationGraphInputView view{inputs, &source, source.worker_specs_.size(),
        [](const void* p, const AdmittedPairwiseProductionInputs& admitted, std::optional<PreparationGraphPolicies>& output) noexcept {
            const auto& v=*static_cast<const PairwiseTypedPreOwnerPreparation*>(p);
            // Kernel configuration is supplied from the actual immutable prepared alternative.
            output.emplace(v.calibration_,v.warmup_,v.repetition_,v.comparison_order_,v.validity_,v.statistics_policy_,
                v.monitoring_policy_, admitted.pairwise_request().first().execution_inputs().kernel(),v.implementation_id_,v.measurement_root_seed_,
                v.resolved_latency_seed_,v.calibration_enabled_);
        },
        [](const void* p, std::size_t i) noexcept -> const WorkerLaunchSpec& {
            return static_cast<const PairwiseTypedPreOwnerPreparation*>(p)->worker_specs_[i];
        },
        [](const void* p, bool second, std::size_t i) noexcept -> const memory::RegionRequest& {
            const auto& v=*static_cast<const PairwiseTypedPreOwnerPreparation*>(p);
            return (second ? v.second_memory_ : v.first_memory_)[i].request;
        },
        [](const void* p, bool second, std::size_t i) noexcept {
            const auto& v=*static_cast<const PairwiseTypedPreOwnerPreparation*>(p);
            return (second ? v.second_memory_ : v.first_memory_)[i].workspace.node_capacity();
        }};
    return prepare_values(view,dimensions,limits,domain);
}
auto PairwisePreparationGraph::prepare_values(const PreparationGraphInputView& source,
    const PreparationGraphDimensions& dimensions, const PreparationGraphLimits& limits,
    MonitoringPreparationFactDomain domain) -> std::expected<PairwisePreparationGraph, PreparationGraphFailure> {
    ConstructionLocals local;
    const auto& inputs=source.inputs;
    const auto& first=inputs.pairwise_request().first().request();
    local.provenance_source=std::get_if<ScenarioPlacementProvenance>(&first.provenance());
    if (!local.provenance_source || !source.backing || !source.policies || !source.worker || !source.request || !source.pages)
        return std::unexpected{PreparationGraphFailure::identity};
    for (local.side=0;local.side<2U;++local.side) {
        std::copy_n("sha256:",7U,local.digest_text[local.side].begin());
        for (local.index=0;local.index<32U;++local.index) {
            local.digest_byte=std::to_integer<unsigned>(local.side
                ? local.provenance_source->resolved_scenario_plan_digest().bytes()[local.index]
                : local.provenance_source->scenario_digest().bytes()[local.index]);
            local.digest_text[local.side][7U+2U*local.index]="0123456789abcdef"[local.digest_byte>>4U];
            local.digest_text[local.side][8U+2U*local.index]="0123456789abcdef"[local.digest_byte&15U];
        }
    }
    local.provenance = {"epycbench.scenario/v0.1", std::string_view{local.digest_text[0].data(),71U},
        std::string_view{local.digest_text[1].data(),71U}, first.experiment().workload_id().value(),
        first.experiment().id().value(), ":first", first.experiment().id().value(), ":second"};
    local.workers = source.workers;
    if (local.workers > std::numeric_limits<std::size_t>::max()/2U)
        return std::unexpected{PreparationGraphFailure::capacity};
    local.rows = 2U * local.workers;
    if (!local.workers || local.workers!=inputs.pairwise_request().first().workers().size())
        return std::unexpected{PreparationGraphFailure::identity};
    for (const auto value : local.provenance) local.strings.add(value.size());
    for (local.index=0;local.index<local.workers;++local.index) {
        local.strings.add(source.worker(source.backing,local.index).stable_pu.value().size());
        local.strings.add(source.worker(source.backing,local.index).stable_core.value().size());
        for (local.side=0;local.side<2U;++local.side) {
            local.targets.add(source.request(source.backing,local.side!=0U,local.index).numa_targets().size());
            local.pages.add(source.pages(source.backing,local.side!=0U,local.index));
            for (const auto& target : source.request(source.backing,local.side!=0U,local.index).numa_targets())
                local.strings.add(target.stable_node.value().size());
        }
    }
    local.labels.add(dimensions.domains, dimensions.label_bytes);
    local.labels.add(dimensions.domains, dimensions.label_bytes);
    local.retained.add(sizeof(State));
    local.retained.add(local.workers, sizeof(PreparationWorkerCell));
    local.retained.add(local.rows, sizeof(PreparationMemoryCell));
    local.retained.add(local.rows, sizeof(PreparationKernelCell));
    local.retained.add(local.targets.value, sizeof(PreparationTargetCell));
    local.retained.add(local.pages.value, sizeof(PreparationPageCountCell));
    local.retained.add(dimensions.domains, sizeof(PreparationFrequencyCell));
    local.retained.add(local.workers, sizeof(core::LinuxCpuId));
    local.retained.add(dimensions.memberships, sizeof(core::LinuxCpuId));
    local.retained.add(local.strings.value); local.retained.add(local.labels.value);
    local.peak.add(local.retained.value); local.peak.add(construction_staging_bytes());
    local.peak.add(2U, sizeof(std::shared_ptr<State>));
    local.peak.add(sizeof(std::expected<PairwisePreparationGraph, PreparationGraphFailure>));
    if (!local.strings.valid || !local.targets.valid || !local.pages.valid || !local.labels.valid ||
        !local.retained.valid || !local.peak.valid || local.workers > limits.workers || local.rows > limits.rows ||
        local.targets.value > limits.targets || local.pages.value > limits.page_counts ||
        dimensions.domains > limits.domains || dimensions.memberships > limits.memberships ||
        local.strings.value > limits.strings || local.labels.value > limits.labels ||
        local.retained.value > limits.retained || local.peak.value > limits.construction ||
        local.workers > std::vector<PreparationWorkerCell>{}.max_size() ||
        local.rows > std::vector<PreparationMemoryCell>{}.max_size() || local.rows > std::vector<PreparationKernelCell>{}.max_size() ||
        local.targets.value > std::vector<PreparationTargetCell>{}.max_size() ||
        local.pages.value > std::vector<PreparationPageCountCell>{}.max_size() ||
        dimensions.domains > std::vector<PreparationFrequencyCell>{}.max_size() ||
        local.workers > std::vector<core::LinuxCpuId>{}.max_size() ||
        dimensions.memberships > std::vector<core::LinuxCpuId>{}.max_size() - local.workers ||
        local.labels.value > std::vector<char>{}.max_size() ||
        local.strings.value > std::vector<char>{}.max_size() - local.labels.value)
        return std::unexpected{PreparationGraphFailure::capacity};
    try {
        auto state = std::make_shared<State>(domain);
        state->usage = {local.retained.value, local.peak.value, local.strings.value, local.targets.value, local.pages.value};
        state->dimensions = dimensions;
        source.policies(source.backing,inputs,state->policies);
        if (!state->policies) return std::unexpected{PreparationGraphFailure::identity};
        state->measurement.emplace(inputs.pairwise_request().first().execution_inputs().measurement());
        state->first_request = inputs.pairwise_request().first().request_id().value(); state->second_request = inputs.pairwise_request().second().request_id().value();
        state->experiment = first.experiment().index();
        state->workers.resize(local.workers); state->memory.resize(local.rows); state->kernels.resize(local.rows);
        state->targets.resize(local.targets.value); state->pages.resize(local.pages.value);
        state->frequency.resize(dimensions.domains);
        state->cpus.resize(local.workers + dimensions.memberships, core::LinuxCpuId{0U});
        state->strings.resize(local.strings.value + local.labels.value);
        for (local.index=0; local.index<local.provenance.size(); ++local.index)
            state->provenance[local.index] = state->copy_text(local.cursor, local.provenance[local.index]);
        for (local.index=0; local.index<local.workers; ++local.index) {
            const auto& worker = source.worker(source.backing,local.index);
            auto& cell = state->workers[local.index];
            cell.index=worker.worker_index; cell.worker=worker.worker_id.value(); cell.requested_cpu=worker.requested_cpu.value();
            cell.pu=state->copy_text(local.cursor,worker.stable_pu.value());
            cell.core=state->copy_text(local.cursor,worker.stable_core.value()); cell.affinity={local.index,1U};
        }
        for (local.side=0; local.side<2U; ++local.side) {
            for (local.index=0; local.index<local.workers; ++local.index) {
                const auto& request=source.request(source.backing,local.side!=0U,local.index);
                auto& cell=state->memory[local.side*local.workers+local.index];
                cell.index=local.index; cell.second=local.side!=0; cell.worker=request.worker_id().value();
                cell.bytes=request.requested_bytes().value(); cell.page_size=request.page_size().value();
                cell.numerator=request.placement_threshold().numerator(); cell.denominator=request.placement_threshold().denominator();
                cell.prefault=request.prefault(); cell.verify_before=request.verify_before(); cell.verify_after=request.verify_after();
                cell.page_mode=request.page_mode(); cell.placement_policy=request.placement_policy();
                cell.targets={local.target,request.numa_targets().size()};
                for (const auto& target : request.numa_targets()) {
                    state->targets[local.target++]={target.linux_node.value(),target.topology_node.value(),
                        state->copy_text(local.cursor,target.stable_node.value())};
                }
                // An absent observation still owns its reserved page-count slice.
                cell.page_storage={local.page,source.pages(source.backing,local.side!=0U,local.index)};
                local.page += source.pages(source.backing,local.side!=0U,local.index);
                state->kernels[local.side*local.workers+local.index]={local.index,cell.worker,cell.second,
                    state->policies->kernel.index()==1U,false};
            }
        }
        for (local.index=0; local.index<dimensions.domains; ++local.index) {
            state->frequency[local.index].driver={local.cursor,0U}; local.cursor+=dimensions.label_bytes;
            state->frequency[local.index].governor={local.cursor,0U}; local.cursor+=dimensions.label_bytes;
        }
        return PairwisePreparationGraph{std::move(state)};
    } catch (const std::bad_alloc&) { return std::unexpected{PreparationGraphFailure::allocation_failure}; }
      catch (const std::length_error&) { return std::unexpected{PreparationGraphFailure::capacity}; }
}
auto PairwisePreparationGraph::state_bytes() noexcept -> std::size_t { return sizeof(State); }
auto PairwisePreparationGraph::construction_staging_bytes() noexcept -> std::size_t {
    // The source adapter remains live while prepare_values builds the graph.
    return sizeof(ConstructionLocals) + sizeof(PreparationGraphInputView);
}
auto PairwisePreparationGraph::capture_staging_bytes() noexcept -> std::size_t {
    return sizeof(PreparationGraphStaging) + sizeof(PreparationSourceCallerStaging) + sizeof(PreparationRequestComparisonStaging) +
        sizeof(PreparationGraphMemoryView) + 2U*sizeof(std::optional<MonitoringPreparationFactView>) +
        2U*sizeof(std::optional<MonitoringPreparationFrequencyView>) + sizeof(MonitoringPreparationFrequencyView);
}
auto PairwisePreparationGraph::comparison_staging_bytes() noexcept -> std::size_t {
    return sizeof(PreparationComparisonStaging) + sizeof(PreparationRequestComparisonStaging) +
        sizeof(PreparationGraphMemoryView) + 2U*sizeof(std::optional<MonitoringPreparationFrequencyView>) +
        sizeof(std::optional<PreparationGraphFactsView>) + sizeof(PreparationGraphFactsView);
}
auto PairwisePreparationGraph::published() const noexcept -> bool { return state_ && !state_->failed && state_->stage==PreparationGraphStage::published; }
auto PairwisePreparationGraph::stable() const noexcept -> bool { return published() || (state_ && state_->retired); }
auto PairwisePreparationGraph::stage() const noexcept -> PreparationGraphStage { return state_ ? state_->stage : PreparationGraphStage::allocated; }
auto PairwisePreparationGraph::domain() const noexcept -> MonitoringPreparationFactDomain { return state_ ? state_->domain : MonitoringPreparationFactDomain::synthetic; }
auto PairwisePreparationGraph::usage() const noexcept -> PreparationGraphUsage { return state_ ? state_->usage : PreparationGraphUsage{}; }
auto PairwisePreparationGraph::lease() const noexcept -> std::shared_ptr<const State> { return stable() ? state_ : nullptr; }
auto PairwisePreparationGraph::worker_count() const noexcept -> std::size_t { return state_ ? state_->workers.size() : 0U; }
auto PairwisePreparationGraph::policies() const noexcept -> const PreparationGraphPolicies* { return stable() && state_->policies ? &*state_->policies : nullptr; }
auto PairwisePreparationGraph::monitoring() const noexcept -> const PreparationMonitoringIdentity* { return stable() && state_->monitoring ? &*state_->monitoring : nullptr; }
auto PairwisePreparationGraph::frequency_count() const noexcept -> std::size_t { return stable() ? state_->frequency_count : 0U; }
auto PairwisePreparationGraph::frequency(std::size_t index) const noexcept -> std::optional<MonitoringPreparationFrequencyView> {
    if (!stable() || index>=state_->frequency_count) return std::nullopt;
    const auto& row=state_->frequency[index];
    return MonitoringPreparationFrequencyView{{state_->cpus.data()+row.cpus.offset,row.cpus.size},row.kind,
        state_->text(row.driver),state_->text(row.governor),row.minimum,row.maximum};
}
auto PreparationGraphFactsView::text(PreparationSlice value) const noexcept -> std::string_view {
    if (value.offset>strings.size() || value.size>strings.size()-value.offset) return {};
    return {strings.data()+value.offset,value.size};
}
auto PairwisePreparationGraph::facts() const noexcept -> std::optional<PreparationGraphFactsView> {
    if (!stable()) return std::nullopt;
    return PreparationGraphFactsView{state_->policies,state_->monitoring,state_->provenance,state_->measurement,
        state_->experiment,state_->first_request,state_->second_request,state_->workers,state_->memory,state_->kernels,
        state_->targets,state_->pages,state_->frequency,state_->cpus,state_->strings,state_->frequency_count};
}
void PairwisePreparationGraph::retire() noexcept { if (state_) state_->retired=true; }
auto PairwisePreparationGraph::reject_capture() noexcept -> bool {
    if (state_ && !state_->retired && state_->stage!=PreparationGraphStage::published) state_->failed=true;
    return false;
}

namespace {
template<class Facts>
auto request_matches(const Facts& state, const PreparationMemoryCell& cell,
                     const memory::RegionRequest& value) noexcept -> bool {
    if (cell.worker!=value.worker_id().value() || cell.bytes!=value.requested_bytes().value() ||
        cell.page_size!=value.page_size().value() || cell.numerator!=value.placement_threshold().numerator() ||
        cell.denominator!=value.placement_threshold().denominator() || cell.prefault!=value.prefault() ||
        cell.verify_before!=value.verify_before() || cell.verify_after!=value.verify_after() ||
        cell.page_mode!=value.page_mode() || cell.placement_policy!=value.placement_policy() ||
        cell.targets.size!=value.numa_targets().size() || cell.targets.offset>state.targets.size() ||
        cell.targets.size>state.targets.size()-cell.targets.offset) return false;
    PreparationRequestComparisonStaging stage;
    for (;stage.index<cell.targets.size;++stage.index) {
        stage.target=&state.targets[cell.targets.offset+stage.index]; stage.other=&value.numa_targets()[stage.index];
        if (stage.target->linux_node!=stage.other->linux_node.value() || stage.target->topology_node!=stage.other->topology_node.value() ||
            state.text(stage.target->stable)!=stage.other->stable_node.value()) return false;
    }
    return true;
}
}
auto PairwisePreparationGraph::matches_inputs(const AdmittedMeasurementInputs& m,
    const StatisticsProductionPolicy& statistics, const monitoring::MonitoringExecutionPolicy& monitoring,
    const AdmittedKernelExecutionInputs& first, const AdmittedKernelExecutionInputs& second,
    std::uint64_t first_request, std::uint64_t second_request, std::uint64_t comparison_seed) const noexcept -> bool {
    if (!published()) return false;
    const auto view=facts();
    return view && view->matches_inputs(m,statistics,monitoring,first,second,first_request,second_request,comparison_seed);
}
auto PairwisePreparationGraph::matches_identity(const PairwiseRunProvenance& value) const noexcept -> bool {
    const auto view=facts();
    return view && view->matches_identity(value);
}
auto PairwisePreparationGraph::matches_worker(std::size_t index,const WorkerReadyEvidence& value) const noexcept -> bool {
    const auto view=facts();
    return view && view->matches_worker(index,value);
}
auto PreparationGraphFactsView::matches_inputs(const AdmittedMeasurementInputs& m,
    const StatisticsProductionPolicy& statistics, const monitoring::MonitoringExecutionPolicy& expected_monitoring,
    const AdmittedKernelExecutionInputs& first, const AdmittedKernelExecutionInputs& second,
    std::uint64_t expected_first_request, std::uint64_t expected_second_request, std::uint64_t comparison_seed) const noexcept -> bool {
    if (!measurement || *this->measurement!=m || !this->policies ||
        this->first_request!=expected_first_request || this->second_request!=expected_second_request) return false;
    PreparationComparisonStaging stage{};
    stage.policy=&*this->policies;
    stage.warmup_returns[0].emplace(stage.policy->warmup.stability_threshold());
    stage.overhead_returns[0].emplace(stage.policy->validity.maximum_harness_overhead());
    const auto& p=*stage.policy;
    const auto same=[](double a,double b) noexcept { return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b); };
    if (p.statistics!=statistics ||
        !same(p.statistics.comparison_bootstrap_policy().confidence().value(),statistics.comparison_bootstrap_policy().confidence().value()) ||
        !same(p.statistics.stopping_bootstrap_policy().confidence().value(),statistics.stopping_bootstrap_policy().confidence().value()) ||
        !same(p.statistics.effective_practical_effect_fraction(),statistics.effective_practical_effect_fraction()) ||
        !same(p.statistics.practical_effect_threshold().delta(),statistics.practical_effect_threshold().delta()) ||
        !same(p.statistics.relative_half_width_target().value(),statistics.relative_half_width_target().value()) ||
        p.monitoring!=expected_monitoring || p.kernel!=first || p.kernel!=second || p.root_seed!=m.root_seed() ||
        p.calibration_enabled!=m.calibration_enabled() || p.comparison.comparison_order_seed()!=comparison_seed ||
        p.calibration.minimum_duration()!=m.observation_minimum_duration() ||
        p.calibration.target_duration()!=m.observation_target_duration() || p.calibration.maximum_duration()!=m.observation_maximum_duration() ||
        p.warmup.minimum_trials()!=m.warmup_minimum() || p.warmup.maximum_trials()!=m.warmup_maximum() ||
        p.warmup.stability_window()!=m.warmup_stability_window() ||
        stage.warmup_returns[0]->numerator()!=m.warmup_stability_threshold().numerator() ||
        stage.warmup_returns[0]->denominator()!=m.warmup_stability_threshold().denominator() ||
        stage.overhead_returns[0]->numerator()!=m.maximum_harness_overhead().numerator() ||
        stage.overhead_returns[0]->denominator()!=m.maximum_harness_overhead().denominator() ||
        p.repetition.minimum_blocks()!=m.repetition_minimum_blocks() || p.repetition.maximum_blocks()!=m.repetition_maximum_blocks() ||
        p.repetition.minimum_valid_observations_per_variant()!=m.minimum_valid_observations_per_variant() ||
        p.repetition.stopping_enabled()!=m.stopping_enabled()) return false;
    switch (m.comparison_order()) {
    case scenario::MatrixComparisonOrder::randomized_abba:
        if (p.comparison.strategy()!=measurement::ComparisonOrderStrategy::randomized_abba) return false;
        break;
    case scenario::MatrixComparisonOrder::randomized_baab:
        if (p.comparison.strategy()!=measurement::ComparisonOrderStrategy::randomized_baab) return false;
        break;
    case scenario::MatrixComparisonOrder::randomized_balanced:
        if (p.comparison.strategy()!=measurement::ComparisonOrderStrategy::randomized_balanced) return false;
        break;
    }
    if (p.implementation.family()!=(first.index()==1U ? kernel::KernelFamily::latency : kernel::KernelFamily::bandwidth)) return false;
    if (first.index()==1U && p.latency_seed!=std::get<AdmittedLatencyExecutionInputs>(first).resolved_permutation_seed()) return false;
    for (const auto& row : this->kernels)
        if (!row.captured || row.index>=this->workers.size() || row.worker!=this->workers[row.index].worker || row.latency!=(first.index()==1U)) return false;
    return true;
}
auto PreparationGraphFactsView::matches_identity(const PairwiseRunProvenance& value) const noexcept -> bool {
    if (experiment!=value.experiment_id.value()) return false;
    const auto& s=*this;
    const auto variant=[&](std::string_view actual,std::size_t suffix) noexcept {
        const auto id=s.text(s.provenance[suffix-1U]); const auto tail=s.text(s.provenance[suffix]);
        return actual.size()==id.size()+tail.size() && actual.substr(0,id.size())==id && actual.substr(id.size())==tail;
    };
    return s.text(s.provenance[0])==value.scenario_profile && s.text(s.provenance[1])==value.scenario_digest &&
        s.text(s.provenance[2])==value.resolved_plan_digest && s.text(s.provenance[3])==value.workload_id &&
        variant(value.first_variant_id,5U) && variant(value.second_variant_id,7U);
}
auto PreparationGraphFactsView::matches_worker(std::size_t index,const WorkerReadyEvidence& value) const noexcept -> bool {
    if (index>=workers.size()) return false;
    PreparationComparisonStaging stage{}; stage.worker=&this->workers[index];
    const auto& row=*stage.worker;
    return row.affinity.size==1U && row.affinity.offset<cpus.size() &&
        row.captured && row.index==value.worker_index && row.worker==value.worker_id.value() &&
        row.requested_cpu==value.requested_cpu.value() && row.observed_cpu==value.observed_cpu.value() &&
        row.tid==value.tid.value() && this->text(row.pu)==value.stable_pu.value() &&
        this->text(row.core)==value.stable_core.value() && value.observed_affinity.cardinality()==1U &&
        this->cpus[row.affinity.offset]==*value.observed_affinity.begin();
}
auto PairwisePreparationGraph::capture_workers(std::span<const WorkerReadyEvidence> rows) noexcept -> bool {
    if (!state_ || state_->retired || state_->failed || state_->stage!=PreparationGraphStage::allocated || rows.size()!=state_->workers.size()) return reject_capture();
    PreparationGraphStaging stage;
    stage.workers=rows; stage.graph=state_;
    for (stage.index=0;stage.index<rows.size();++stage.index) {
        const auto& a=state_->workers[stage.index]; const auto& b=rows[stage.index];
        if (a.index!=b.worker_index || a.worker!=b.worker_id.value() || a.requested_cpu!=b.requested_cpu.value() ||
            state_->text(a.pu)!=b.stable_pu.value() || state_->text(a.core)!=b.stable_core.value() ||
            b.observed_affinity.cardinality()!=1U || *b.observed_affinity.begin()!=b.requested_cpu ||
            b.observed_cpu!=b.requested_cpu || b.tid.value()==0) return reject_capture();
    }
    for (stage.index=0;stage.index<rows.size();++stage.index) {
        auto& row=state_->workers[stage.index]; row.tid=rows[stage.index].tid.value(); row.observed_cpu=rows[stage.index].observed_cpu.value();
        state_->cpus[row.affinity.offset]=*rows[stage.index].observed_affinity.begin(); row.captured=true;
    }
    state_->stage=PreparationGraphStage::workers; return true;
}
auto PairwisePreparationGraph::capture_memory(bool second,std::size_t index,PreparationGraphMemoryView value) noexcept -> bool {
    if (!state_ || state_->retired || state_->failed || index>=state_->workers.size() ||
        state_->stage!=(second?PreparationGraphStage::first_memory:PreparationGraphStage::workers)) return reject_capture();
    auto& row=state_->memory[(second?state_->workers.size():0U)+index];
    if (row.captured || !request_matches(*state_,row,value.request) ||
        (value.observation && value.observation->resident_page_counts().size()>row.page_storage.size)) return reject_capture();
    PreparationGraphStaging stage;
    stage.memory=&value; stage.graph=state_;
    row.binding=value.binding; row.initialization=value.initialization; row.query=value.query;
    row.qualification=value.qualification; row.protection=value.protection;
    if (value.observation) {
        row.observation.emplace(value.observation->total_pages(),value.observation->resident_pages(),
            value.observation->unresolved_pages(),PreparationSlice{row.page_storage.offset,value.observation->resident_page_counts().size()});
        for (;stage.index<row.observation->counts.size;++stage.index) {
            const auto& count=value.observation->resident_page_counts()[stage.index];
            state_->pages[row.page_storage.offset+stage.index]={count.linux_node().value(),count.page_count()};
        }
    }
    if (value.evaluation) row.evaluation.emplace(value.evaluation->requested_node_pages(),value.evaluation->unexpected_node_pages(),
        value.evaluation->unresolved_pages(),value.evaluation->observed_ratio().numerator(),value.evaluation->observed_ratio().denominator());
    row.captured=true; return true;
}
auto PairwisePreparationGraph::matches_memory(bool second,std::size_t index,PreparationGraphMemoryView value) const noexcept -> bool {
    if (!stable() || state_->stage<(second?PreparationGraphStage::second_memory:PreparationGraphStage::first_memory)) return false;
    const auto view=facts();
    return view && view->matches_memory(second,index,value);
}
auto PreparationGraphFactsView::matches_memory(bool second,std::size_t index,PreparationGraphMemoryView value) const noexcept -> bool {
    if (index>=workers.size() || workers.size()>memory.size()/2U) return false;
    PreparationComparisonStaging stage{}; stage.memory=&this->memory[(second?this->workers.size():0U)+index];
    const auto& row=*stage.memory;
    if (!row.captured || !row.binding || !row.initialization || !row.protection ||
        !request_matches(*this,row,value.request) || row.query!=value.query || row.qualification!=value.qualification ||
        bool(row.observation)!=bool(value.observation) || bool(row.evaluation)!=bool(value.evaluation) ||
        row.initialization->prefault_requested()!=value.initialization.prefault_requested() ||
        row.initialization->prefault_pass_performed()!=value.initialization.prefault_pass_performed()) return false;
    if (row.binding->policy()!=value.binding.policy() || row.binding->range()!=value.binding.range() ||
        row.binding->policy_call_accepted()!=value.binding.policy_call_accepted() ||
        row.binding->migration_requested()!=value.binding.migration_requested() ||
        row.binding->strict_verification_requested()!=value.binding.strict_verification_requested() ||
        row.initialization->range()!=value.initialization.range() ||
        row.initialization->initialization_completed()!=value.initialization.initialization_completed() ||
        row.initialization->padding_touched()!=value.initialization.padding_touched() ||
        row.protection->protection_kind()!=value.protection.protection_kind() || row.protection->range()!=value.protection.range() ||
        row.protection->protection_accepted()!=value.protection.protection_accepted() || row.protection->writable()!=value.protection.writable() ||
        row.protection->executable()!=value.protection.executable()) return false;
    if (value.observation) {
        if (row.observation->counts.offset>pages.size() || row.observation->counts.size>pages.size()-row.observation->counts.offset ||
            row.observation->total!=value.observation->total_pages() || row.observation->resident!=value.observation->resident_pages() ||
            row.observation->unresolved!=value.observation->unresolved_pages() ||
            row.observation->counts.size!=value.observation->resident_page_counts().size()) return false;
        for (;stage.index<row.observation->counts.size;++stage.index) {
            const auto& a=this->pages[row.observation->counts.offset+stage.index]; const auto& b=value.observation->resident_page_counts()[stage.index];
            if (a.node!=b.linux_node().value() || a.count!=b.page_count()) return false;
        }
    }
    return !value.evaluation || (row.evaluation->requested==value.evaluation->requested_node_pages() &&
        row.evaluation->unexpected==value.evaluation->unexpected_node_pages() && row.evaluation->unresolved==value.evaluation->unresolved_pages() &&
        row.evaluation->numerator==value.evaluation->observed_ratio().numerator() && row.evaluation->denominator==value.evaluation->observed_ratio().denominator());
}
auto PairwisePreparationGraph::capture_kernel(bool second,std::size_t index,bool latency) noexcept -> bool {
    if (!state_ || state_->retired || state_->failed || state_->stage!=PreparationGraphStage::second_memory || index>=state_->workers.size()) return reject_capture();
    auto& row=state_->kernels[(second?state_->workers.size():0U)+index];
    if (row.captured || row.latency!=latency) return reject_capture();
    row.captured=true; return true;
}
auto PairwisePreparationGraph::transition(PreparationGraphStage next) noexcept -> bool {
    if (!state_ || state_->retired || state_->failed || static_cast<unsigned>(next)!=static_cast<unsigned>(state_->stage)+1U) return reject_capture();
    if (next==PreparationGraphStage::first_memory || next==PreparationGraphStage::second_memory) {
        const auto offset=next==PreparationGraphStage::second_memory ? state_->workers.size():0U;
        for (std::size_t i{};i<state_->workers.size();++i) if (!state_->memory[offset+i].captured) return reject_capture();
    } else if (next==PreparationGraphStage::kernels) {
        for (const auto& row:state_->kernels) if (!row.captured) return reject_capture();
    } else if (next!=PreparationGraphStage::transferred && next!=PreparationGraphStage::bound && next!=PreparationGraphStage::published) return reject_capture();
    state_->stage=next; return true;
}
auto PairwisePreparationGraph::capture_monitoring(const MonitoringPreparationFactView& view) noexcept -> bool {
    if (!state_ || state_->retired || state_->failed || state_->stage!=PreparationGraphStage::kernels || view.domain()!=state_->domain ||
        view.admission().monitoring_policy()!=state_->policies->monitoring || view.workers().size()!=state_->workers.size() ||
        view.frequency_domains()>state_->dimensions.domains) return reject_capture();
    PreparationGraphStaging stage;
    stage.monitoring=&view; stage.graph=state_;
    for (stage.index=0;stage.index<view.workers().size();++stage.index)
        if (view.workers()[stage.index].worker_id().value()!=state_->workers[stage.index].worker ||
            view.workers()[stage.index].linux_cpu().value()!=state_->workers[stage.index].requested_cpu ||
            view.workers()[stage.index].operational_thread_id().value()!=state_->workers[stage.index].tid) return reject_capture();
    for (stage.index=0;stage.index<view.frequency_domains();++stage.index) {
        stage.frequency=view.frequency(stage.index);
        if (!stage.frequency || stage.frequency->cpus.size()>state_->dimensions.memberships-stage.members || stage.frequency->driver.size()>state_->dimensions.label_bytes ||
            stage.frequency->governor.size()>state_->dimensions.label_bytes) return reject_capture();
        stage.members+=stage.frequency->cpus.size();
    }
    const auto& authority=view.admission().authority();
    state_->monitoring.emplace(view.channels(),view.service(),authority.physical_fingerprint(),authority.effective_fingerprint(),
        authority.provenance(),view.admission().capacity_contract_identity(),view.admission().finalized_error_profile_identity());
    stage.members=state_->workers.size();
    for (stage.index=0;stage.index<view.frequency_domains();++stage.index) {
        stage.frequency=view.frequency(stage.index); auto& target=state_->frequency[stage.index];
        target.kind=stage.frequency->kind; target.minimum=stage.frequency->minimum_hz; target.maximum=stage.frequency->maximum_hz;
        target.cpus={stage.members,stage.frequency->cpus.size()};
        std::copy(stage.frequency->cpus.begin(),stage.frequency->cpus.end(),state_->cpus.begin()+static_cast<std::ptrdiff_t>(stage.members)); stage.members+=stage.frequency->cpus.size();
        target.driver.size=stage.frequency->driver.size(); target.governor.size=stage.frequency->governor.size();
        std::copy(stage.frequency->driver.begin(),stage.frequency->driver.end(),state_->strings.begin()+static_cast<std::ptrdiff_t>(target.driver.offset));
        std::copy(stage.frequency->governor.begin(),stage.frequency->governor.end(),state_->strings.begin()+static_cast<std::ptrdiff_t>(target.governor.offset));
    }
    state_->frequency_count=view.frequency_domains(); state_->stage=PreparationGraphStage::monitoring; return true;
}
auto PairwisePreparationGraph::matches_monitoring(const MonitoringPreparationFactView& view) const noexcept -> bool {
    if (!stable() || !state_->monitoring || view.domain()!=state_->domain || view.admission().monitoring_policy()!=state_->policies->monitoring ||
        view.workers().size()!=state_->workers.size() || view.frequency_domains()!=state_->frequency_count ||
        view.channels()!=state_->monitoring->channels || view.service()!=state_->monitoring->service) return false;
    const auto& authority=view.admission().authority();
    if (authority.physical_fingerprint()!=state_->monitoring->physical || authority.effective_fingerprint()!=state_->monitoring->effective ||
        authority.provenance()!=state_->monitoring->provenance || view.admission().capacity_contract_identity()!=state_->monitoring->capacity ||
        view.admission().finalized_error_profile_identity()!=state_->monitoring->profile) return false;
    for (std::size_t i{};i<view.workers().size();++i)
        if (view.workers()[i].worker_id().value()!=state_->workers[i].worker || view.workers()[i].linux_cpu().value()!=state_->workers[i].requested_cpu ||
            view.workers()[i].operational_thread_id().value()!=state_->workers[i].tid) return false;
    for (std::size_t i{};i<view.frequency_domains();++i) {
        const auto a=frequency(i),b=view.frequency(i);
        if (!a || !b || !std::ranges::equal(a->cpus,b->cpus) || a->kind!=b->kind || a->driver!=b->driver ||
            a->governor!=b->governor || a->minimum_hz!=b->minimum_hz || a->maximum_hz!=b->maximum_hz) return false;
    }
    return true;
}
} // namespace epycbench::runtime::detail
