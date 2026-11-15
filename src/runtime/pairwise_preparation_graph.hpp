#pragma once

#include "runtime/pairwise_monitoring_preparation_facts.hpp"
#include "epycbench/runtime/pairwise_run_evidence.hpp"
#include "epycbench/runtime/worker_execution.hpp"
#include "epycbench/memory/region_protection.hpp"
#include <memory>
#include "epycbench/runtime/production_admission.hpp"
#include "epycbench/runtime/pairwise_runner_execution_backend.hpp"

namespace epycbench::runtime::detail {
struct WorkerLaunchSpec;
class PairwiseTypedPreOwnerPreparation;
class PairwiseTypedOwnerTransaction;
class PairwiseTypedProductionBinding;
struct PreparationGraphTestAccess;

struct PreparationSlice final {
    std::size_t offset{}, size{};
    auto operator==(const PreparationSlice&) const noexcept -> bool = default;
};
struct PreparationTargetCell final {
    std::uint32_t linux_node{}, topology_node{};
    PreparationSlice stable;
};
struct PreparationPageCountCell final { std::uint32_t node{}; std::uint64_t count{}; };
struct PreparationWorkerCell final {
    std::size_t index{};
    std::uint64_t worker{}, tid{};
    std::uint32_t requested_cpu{}, observed_cpu{};
    PreparationSlice pu, core, affinity;
    bool captured{};
};
struct PreparationObservationCell final {
    std::uint64_t total{}, resident{}, unresolved{};
    PreparationSlice counts;
};
struct PreparationEvaluationCell final {
    std::uint64_t requested{}, unexpected{}, unresolved{}, numerator{}, denominator{};
};
struct PreparationMemoryCell final {
    std::size_t index{};
    std::uint64_t worker{}, bytes{}, page_size{}, numerator{}, denominator{};
    bool second{}, prefault{}, verify_before{}, verify_after{}, captured{};
    memory::PageMode page_mode{};
    memory::PlacementPolicy placement_policy{};
    PreparationSlice targets, page_storage;
    std::optional<memory::RegionBindingEvidence> binding;
    std::optional<memory::RegionInitializationEvidence> initialization;
    memory::PreRunPlacementQueryState query{};
    std::optional<PreparationObservationCell> observation;
    std::optional<PreparationEvaluationCell> evaluation;
    memory::PlacementQualificationState qualification{};
    std::optional<memory::RegionProtectionEvidence> protection;
};
struct PreparationKernelCell final {
    std::size_t index{};
    std::uint64_t worker{};
    bool second{}, latency{}, captured{};
};
struct PreparationFrequencyCell final {
    PreparationSlice cpus, driver, governor;
    monitoring::FrequencySourceKind kind{};
    std::uint64_t minimum{}, maximum{};
};
struct PreparationGraphPolicies final {
    measurement::CalibrationDurationPolicy calibration;
    measurement::WarmupPolicy warmup;
    measurement::RepetitionPolicy repetition;
    measurement::ComparisonOrderPolicy comparison;
    measurement::ObservationValidityPolicy validity;
    StatisticsProductionPolicy statistics;
    monitoring::MonitoringExecutionPolicy monitoring;
    AdmittedKernelExecutionInputs kernel;
    kernel::KernelImplementationId implementation;
    std::uint64_t root_seed{}, latency_seed{};
    bool calibration_enabled{};
};
struct PreparationMonitoringIdentity final {
    std::array<MonitoringPreparationChannelFact, 4> channels;
    std::optional<MonitoringPreparationServiceFact> service;
    topology::PhysicalTopologyFingerprint physical;
    topology::EffectiveTopologyFingerprint effective;
    platform_linux::detail::LinuxMonitoringCapacityProvenance provenance;
    core::Sha256Digest capacity, profile;
};
struct PreparationGraphInputView final {
    const AdmittedPairwiseProductionInputs& inputs;
    const void* backing;
    std::size_t workers;
    void (*policies)(const void*, const AdmittedPairwiseProductionInputs&, std::optional<PreparationGraphPolicies>&) noexcept;
    const WorkerLaunchSpec& (*worker)(const void*, std::size_t) noexcept;
    const memory::RegionRequest& (*request)(const void*, bool, std::size_t) noexcept;
    std::size_t (*pages)(const void*, bool, std::size_t) noexcept;
};
struct PreparationGraphDimensions final { std::size_t domains{}, memberships{}, label_bytes{}; };
struct PreparationGraphLimits final {
    std::size_t workers{}, rows{}, targets{}, page_counts{}, domains{}, memberships{}, strings{}, labels{};
    std::size_t retained{}, construction{};
};
enum class PreparationGraphFailure : unsigned char { capacity, state, identity, malformed, allocation_failure };
enum class PreparationGraphStage : unsigned char { allocated, workers, first_memory, second_memory, kernels, monitoring, transferred, bound, published };
struct PreparationGraphUsage final { std::size_t retained{}, construction{}, strings{}, targets{}, page_counts{}; };
struct PreparationGraphMemoryView final {
    const memory::RegionRequest& request;
    const memory::RegionBindingEvidence& binding;
    const memory::RegionInitializationEvidence& initialization;
    memory::PreRunPlacementQueryState query;
    const std::optional<memory::RegionPlacementObservation>& observation;
    const std::optional<memory::RegionPlacementEvaluation>& evaluation;
    memory::PlacementQualificationState qualification;
    const memory::RegionProtectionEvidence& protection;
};

// Borrowed immutable factual projection. It conveys no capture/publication authority.
// Decoded users must validate the arena dimensions before retaining this view.
struct PreparationGraphFactsView final {
    const std::optional<PreparationGraphPolicies>& policies;
    const std::optional<PreparationMonitoringIdentity>& monitoring;
    const std::array<PreparationSlice, 8>& provenance;
    const std::optional<AdmittedMeasurementInputs>& measurement;
    std::uint64_t experiment, first_request, second_request;
    std::span<const PreparationWorkerCell> workers;
    std::span<const PreparationMemoryCell> memory;
    std::span<const PreparationKernelCell> kernels;
    std::span<const PreparationTargetCell> targets;
    std::span<const PreparationPageCountCell> pages;
    std::span<const PreparationFrequencyCell> frequency;
    std::span<const core::LinuxCpuId> cpus;
    std::span<const char> strings;
    std::size_t frequency_count;
    [[nodiscard]] auto text(PreparationSlice) const noexcept -> std::string_view;
    [[nodiscard]] auto matches_inputs(const AdmittedMeasurementInputs&, const StatisticsProductionPolicy&,
        const monitoring::MonitoringExecutionPolicy&, const AdmittedKernelExecutionInputs&,
        const AdmittedKernelExecutionInputs&, std::uint64_t, std::uint64_t, std::uint64_t) const noexcept -> bool;
    [[nodiscard]] auto matches_identity(const PairwiseRunProvenance&) const noexcept -> bool;
    [[nodiscard]] auto matches_worker(std::size_t, const WorkerReadyEvidence&) const noexcept -> bool;
    [[nodiscard]] auto matches_memory(bool, std::size_t, PreparationGraphMemoryView) const noexcept -> bool;
};

class PairwisePreparationGraph final {
  public:
    struct State;
    PairwisePreparationGraph(PairwisePreparationGraph&&) noexcept = default;
    auto operator=(PairwisePreparationGraph&&) noexcept -> PairwisePreparationGraph& = default;
    PairwisePreparationGraph(const PairwisePreparationGraph&) = delete;
    [[nodiscard]] auto published() const noexcept -> bool;
    [[nodiscard]] auto stable() const noexcept -> bool;
    [[nodiscard]] auto stage() const noexcept -> PreparationGraphStage;
    [[nodiscard]] auto domain() const noexcept -> MonitoringPreparationFactDomain;
    [[nodiscard]] auto usage() const noexcept -> PreparationGraphUsage;
    [[nodiscard]] auto retain() const noexcept -> PairwisePreparationGraph { return PairwisePreparationGraph{state_}; }
    [[nodiscard]] auto lease() const noexcept -> std::shared_ptr<const State>;
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;
    [[nodiscard]] auto facts() const noexcept -> std::optional<PreparationGraphFactsView>;
    [[nodiscard]] auto matches_worker(std::size_t, const WorkerReadyEvidence&) const noexcept -> bool;
    [[nodiscard]] auto matches_memory(bool, std::size_t, PreparationGraphMemoryView) const noexcept -> bool;
    [[nodiscard]] auto matches_monitoring(const MonitoringPreparationFactView&) const noexcept -> bool;
    [[nodiscard]] auto policies() const noexcept -> const PreparationGraphPolicies*;
    [[nodiscard]] auto monitoring() const noexcept -> const PreparationMonitoringIdentity*;
    [[nodiscard]] auto frequency_count() const noexcept -> std::size_t;
    [[nodiscard]] auto frequency(std::size_t) const noexcept -> std::optional<MonitoringPreparationFrequencyView>;
    [[nodiscard]] auto matches_inputs(const AdmittedMeasurementInputs&, const StatisticsProductionPolicy&,
        const monitoring::MonitoringExecutionPolicy&, const AdmittedKernelExecutionInputs&,
        const AdmittedKernelExecutionInputs&, std::uint64_t, std::uint64_t, std::uint64_t) const noexcept -> bool;
    [[nodiscard]] static auto state_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto construction_staging_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto capture_staging_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto comparison_staging_bytes() noexcept -> std::size_t;
    [[nodiscard]] auto matches_identity(const PairwiseRunProvenance&) const noexcept -> bool;
  private:
    explicit PairwisePreparationGraph(std::shared_ptr<State> state) noexcept : state_{std::move(state)} {}
    static auto prepare(const PairwiseTypedPreOwnerPreparation&, const AdmittedPairwiseProductionInputs&, const PreparationGraphDimensions&,
                        const PreparationGraphLimits&, MonitoringPreparationFactDomain)
        -> std::expected<PairwisePreparationGraph, PreparationGraphFailure>;
    static auto prepare_values(const PreparationGraphInputView&, const PreparationGraphDimensions&,
        const PreparationGraphLimits&, MonitoringPreparationFactDomain)
        -> std::expected<PairwisePreparationGraph, PreparationGraphFailure>;
    auto capture_workers(std::span<const WorkerReadyEvidence>) noexcept -> bool;
    auto capture_memory(bool, std::size_t, PreparationGraphMemoryView) noexcept -> bool;
    auto capture_kernel(bool, std::size_t, bool) noexcept -> bool;
    auto capture_monitoring(const MonitoringPreparationFactView&) noexcept -> bool;
    auto transition(PreparationGraphStage) noexcept -> bool;
    void retire() noexcept;
    auto reject_capture() noexcept -> bool;
    std::shared_ptr<State> state_;
    friend auto prepare_pairwise_typed_pre_owner(const AdmittedPairwiseProductionInputs&,
        const struct PairwiseTypedPreOwnerDependencies&, std::optional<PreparationGraphLimits>) noexcept
        -> PairwiseOperationResult<PairwiseTypedPreOwnerPreparation>;
    friend class PairwiseTypedPreOwnerPreparation;
    friend class PairwiseTypedOwnerTransaction;
    friend class PairwiseTypedProductionBinding;
    friend class PreparedPairwiseRunnerExecution;
    friend struct PreparationGraphTestAccess;
};
#if defined(EPYCBENCH_PREPARATION_GRAPH_TEST)
// Only synthetic provenance is available from supplied-memory recording.
struct PreparationGraphTestAccess final {
    static auto prepare(const PreparationGraphInputView& input, const PreparationGraphDimensions& dimensions,
        const PreparationGraphLimits& limits) {
        return PairwisePreparationGraph::prepare_values(input, dimensions, limits, MonitoringPreparationFactDomain::synthetic);
    }
    static auto workers(PairwisePreparationGraph& g, std::span<const WorkerReadyEvidence> rows) noexcept { return g.capture_workers(rows); }
    static auto memory(PairwisePreparationGraph& g, bool second, std::size_t index, PreparationGraphMemoryView row) noexcept {
        return g.capture_memory(second,index,row);
    }
    static auto kernel(PairwisePreparationGraph& g, bool second, std::size_t index, bool latency) noexcept { return g.capture_kernel(second,index,latency); }
    static auto monitoring(PairwisePreparationGraph& g, const MonitoringPreparationFactView& view) noexcept { return g.capture_monitoring(view); }
    static auto transition(PairwisePreparationGraph& g, PreparationGraphStage stage) noexcept { return g.transition(stage); }
    static void retire(PairwisePreparationGraph& g) noexcept { g.retire(); }
};
#endif
} // namespace epycbench::runtime::detail
