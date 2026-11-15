#pragma once
#include "results/incremental_block_evidence.hpp"
#include "epycbench/runtime/worker_execution.hpp"
#include "epycbench/runtime/production_control_monitor.hpp"
#include "runtime/finalized_evidence_wire_typed_stock.hpp"
#include "runtime/pairwise_monitoring_preparation_facts.hpp"
#include "runtime/pairwise_preparation_graph.hpp"
namespace epycbench::results::block_evidence {
struct MemoryPreparationRow final {
    memory::RegionRequest request;
    memory::RegionBindingEvidence binding;
    memory::RegionInitializationEvidence initialization;
    memory::PreRunDiagnosticState state;
    std::optional<memory::RegionPlacementObservation> observation;
    std::optional<memory::RegionPlacementEvaluation> evaluation;
    std::optional<memory::PlacementQualificationState> qualification;
    memory::RegionProtectionEvidence protection;
};
struct MemoryPreparationLimits final {
    std::size_t workers{}, targets{}, pages{}, strings{}, typed_payload{};
};
struct MemoryPreparationUsage final {
    std::size_t workers{}, targets{}, pages{}, strings{}, payload{};
};
struct MemoryPreparationEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    std::vector<MemoryPreparationRow> workers;
    MemoryPreparationUsage usage;
};
struct ControlReadinessEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    runtime::ServiceThreadReadyEvidence evidence;
};
struct ReadinessEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    std::vector<runtime::WorkerReadyEvidence> workers;
};
struct ReadinessLimits final {
    std::size_t workers{}, strings{};
};
struct FinalizationLimits final {
    std::size_t workers{}, targets{}, pages{}, typed_payload{}, reconstruction_scratch{};
};
struct FinalizationUsage final {
    std::size_t phase1{}, phase2a{}, phase2b{}, payload{}, scratch{}, workers{}, targets{}, pages{};
};
struct FinalizationEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    runtime::PairwiseFinalizationEvidence evidence;
    std::string canonical;
    FinalizationUsage usage;
};
struct MonitoringPreparationWorker final { core::WorkerId worker; core::LinuxCpuId cpu; };
struct MonitoringPreparationFrequency final {
    std::vector<core::LinuxCpuId> cpus;
    monitoring::FrequencySourceKind kind;
    std::string driver, governor;
    std::uint64_t minimum_hz, maximum_hz;
};
struct MonitoringPreparationLimits final { std::size_t workers{}, domains{}, memberships{}, strings{}; };
struct MonitoringPreparationEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    runtime::detail::MonitoringPreparationFactDomain domain;
    monitoring::MonitoringExecutionPolicy policy;
    std::array<runtime::detail::MonitoringPreparationChannelFact,4> channels;
    std::optional<runtime::detail::MonitoringPreparationServiceFact> service;
    topology::PhysicalTopologyFingerprint physical;
    topology::EffectiveTopologyFingerprint effective;
    platform_linux::detail::LinuxMonitoringCapacityProvenance provenance;
    core::Sha256Digest capacity, profile;
    std::vector<MonitoringPreparationWorker> workers;
    std::vector<MonitoringPreparationFrequency> frequency;
};
// Factual borrows for the shared admission algorithm; no host admission capability.
struct MonitoringFactsView final {
    runtime::detail::MonitoringPreparationFactDomain domain_value;
    const monitoring::MonitoringExecutionPolicy& policy;
    const std::array<runtime::detail::MonitoringPreparationChannelFact,4>& channel_values;
    const std::optional<runtime::detail::MonitoringPreparationServiceFact>& service_value;
    const topology::PhysicalTopologyFingerprint& physical;
    const topology::EffectiveTopologyFingerprint& effective;
    const platform_linux::detail::LinuxMonitoringCapacityProvenance& provenance;
    const core::Sha256Digest& capacity;
    const core::Sha256Digest& profile;
    std::span<const monitoring::MonitoringWorkerScope> worker_values;
    std::size_t domains;
    const void* backing;
    std::optional<runtime::detail::MonitoringPreparationFrequencyView> (*accessor)(const void*,std::size_t) noexcept;
    auto domain() const noexcept { return domain_value; }
    auto channels() const noexcept -> const auto& { return channel_values; }
    auto service() const noexcept -> const auto& { return service_value; }
    auto workers() const noexcept { return worker_values; }
    auto frequency_domains() const noexcept { return domains; }
    auto frequency(std::size_t index) const noexcept -> std::optional<runtime::detail::MonitoringPreparationFrequencyView> {
        return backing && accessor && index<domains ? accessor(backing,index) : std::nullopt;
    }
};
struct DecodedPreparationGraph final {
    std::optional<runtime::detail::PreparationGraphPolicies> policies;
    std::optional<runtime::detail::PreparationMonitoringIdentity> monitoring;
    std::array<runtime::detail::PreparationSlice,8> provenance;
    std::optional<runtime::ipc::PlacementMeasurementInputs> measurement;
    std::uint64_t experiment{},first_request{},second_request{};
    std::vector<runtime::detail::PreparationWorkerCell> workers;
    std::vector<runtime::detail::PreparationMemoryCell> memory;
    std::vector<runtime::detail::PreparationKernelCell> kernels;
    std::vector<runtime::detail::PreparationTargetCell> targets;
    std::vector<runtime::detail::PreparationPageCountCell> pages;
    std::vector<runtime::detail::PreparationFrequencyCell> frequency;
    std::vector<core::LinuxCpuId> cpus;
    std::vector<char> strings;
    std::size_t frequency_count{},retained{};
    runtime::detail::MonitoringPreparationFactDomain domain{runtime::detail::MonitoringPreparationFactDomain::synthetic};
};
// Immutable event backing. Decoding cannot mint the actual graph alternative.
class PreparationFactsLease final {
  public:
    explicit PreparationFactsLease(const runtime::detail::PairwisePreparationGraph& value) noexcept : value_{value.retain()} {}
    PreparationFactsLease(PreparationFactsLease&&) noexcept = default;
    auto operator=(PreparationFactsLease&&) noexcept -> PreparationFactsLease& = default;
    PreparationFactsLease(const PreparationFactsLease&)=delete;
    auto retain() const noexcept -> PreparationFactsLease;
    auto decoded() const noexcept -> bool { return value_.index()==1U; }
    auto published() const noexcept -> bool;
    auto domain() const noexcept -> runtime::detail::MonitoringPreparationFactDomain;
    auto facts() const noexcept -> std::optional<runtime::detail::PreparationGraphFactsView>;
    auto policies() const noexcept -> const runtime::detail::PreparationGraphPolicies*;
    auto monitoring() const noexcept -> const runtime::detail::PreparationMonitoringIdentity*;
    auto frequency_count() const noexcept -> std::size_t;
    auto frequency(std::size_t) const noexcept -> std::optional<runtime::detail::MonitoringPreparationFrequencyView>;
    auto retained_bytes() const noexcept -> std::size_t;
  private:
    explicit PreparationFactsLease(std::shared_ptr<const DecodedPreparationGraph> value) noexcept : value_{std::move(value)} {}
    std::variant<runtime::detail::PairwisePreparationGraph,std::shared_ptr<const DecodedPreparationGraph>> value_;
    friend struct event_profile::detail::PairContextDecoder;
};
struct PreparationAssociationEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    std::array<std::uint64_t,5> references;
    PreparationFactsLease graph;
};
struct PreparationOutcomeLimits final {
    std::size_t candidates{}, trials{}, calibration_payload{}, calibration_scratch{}, warmup_payload{}, warmup_scratch{};
};
struct PreparationOutcomeUsage final {
    std::size_t parse{}, reconstruct{}, projection{}, payload{}, scratch{}, candidates{}, trials{};
};
struct CalibrationEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    std::uint64_t preparation_sequence;
    runtime::PairwiseCalibrationEvidence evidence;
    std::string canonical;
    PreparationOutcomeUsage usage;
};
struct WarmupEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    std::uint64_t calibration_sequence;
    runtime::PairwiseWarmupEvidence evidence;
    std::string canonical;
    PreparationOutcomeUsage usage;
};
struct PairLimits final {
    std::size_t blocks{}, observations{}, events{}, retained_bytes{}, simultaneous_bytes{};
    Limits component{};
    std::size_t root_bytes{}, root_nodes{}, root_strings{}, root_elements{}, root_depth{};
    runtime::wire_v1::detail::RootReconstructionLimits root;
    std::size_t terminal_errors{};
    FinalizationLimits finalization{};
    ReadinessLimits readiness{};
    MemoryPreparationLimits memory_preparation{};
    std::size_t control_strings{};
    MonitoringPreparationLimits monitoring_preparation{};
    PreparationOutcomeLimits preparation_outcomes{};
};
struct PairUsage final {
    std::size_t retained{}, parse{}, reconstruct{}, fragments{}, assembly{}, projection{};
};
struct StoppingUsage final {
    std::size_t phase1{}, phase2{}, payload{};
};
struct StoppingEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    runtime::RunnerStoppingEvaluationEvidence evidence;
    std::string canonical;
    StoppingUsage usage;
};
struct TerminalEvent final {
    std::uint64_t sequence;
    journal::Timestamp timestamp;
    runtime::PairwiseRunTerminalEvidence evidence;
};
class PairPrefix final {
  public:
    struct State;
    static auto prepare(ExpectedPairContext&&, Domain, PairLimits)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_stopping(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_terminal(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_finalization(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_worker_readiness(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_memory_preparation(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_control_readiness(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_monitoring_preparation(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_complete_preparation(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_preparation_outcomes(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy)
        -> std::expected<PairPrefix, Error>;
    static auto prepare_with_observation_timing(ExpectedPairContext&&, Domain, PairLimits,
        runtime::StatisticsProductionPolicy, measurement::RepetitionPolicy, observation_timing::Limits)
        -> std::expected<PairPrefix, Error>;
    auto factual(std::uint64_t, const journal::Timestamp&, std::string_view, observation_timing::Input&)
        -> std::expected<void, Error>;
    auto failed(std::uint64_t, const journal::Timestamp&, std::string_view, observation_timing::Input&)
        -> std::expected<void, Error>;
    [[nodiscard]] auto timing_coverage() const noexcept -> observation_timing::Coverage;
    [[nodiscard]] auto timing_usage() const noexcept -> observation_timing::Usage;
    [[nodiscard]] static auto timing_staging_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto timing_factory_staging_bytes() noexcept -> std::size_t;
    auto calibration(std::uint64_t, const journal::Timestamp&, std::string_view)
        -> std::expected<void, Error>;
    auto warmup(std::uint64_t, const journal::Timestamp&, std::string_view)
        -> std::expected<void, Error>;
    [[nodiscard]] auto calibration_event() const noexcept -> const CalibrationEvent*;
    [[nodiscard]] auto warmup_event() const noexcept -> const WarmupEvent*;
    [[nodiscard]] static auto preparation_outcome_staging_bytes() noexcept -> std::size_t;
    auto preparation_association(std::uint64_t, const journal::Timestamp&,
        const runtime::detail::PairwisePreparationGraph&) -> std::expected<void, Error>;
    [[nodiscard]] auto preparation_event() const noexcept -> const PreparationAssociationEvent*;
    [[nodiscard]] static auto preparation_staging_bytes() noexcept -> std::size_t;
    auto monitoring_preparation(std::uint64_t, const journal::Timestamp&,
        const runtime::detail::MonitoringPreparationFactView&) -> std::expected<void, Error>;
    [[nodiscard]] auto monitoring_preparation_event() const noexcept -> const MonitoringPreparationEvent*;
    [[nodiscard]] static auto monitoring_preparation_staging_bytes() noexcept -> std::size_t;
    auto control_readiness(std::uint64_t, const journal::Timestamp&,
        const runtime::ServiceThreadReadyEvidence&) -> std::expected<void, Error>;
    [[nodiscard]] auto control_readiness_event() const noexcept -> const ControlReadinessEvent*;
    [[nodiscard]] static auto control_readiness_staging_bytes() noexcept -> std::size_t;
    auto memory_preparation(std::uint64_t, const journal::Timestamp&, bool second,
        std::span<const MemoryPreparationRow>) -> std::expected<void, Error>;
    [[nodiscard]] auto memory_preparation_event(bool second) const noexcept -> const MemoryPreparationEvent*;
    [[nodiscard]] static auto memory_preparation_staging_bytes() noexcept -> std::size_t;
    auto worker_readiness(std::uint64_t, const journal::Timestamp&,
        std::span<const runtime::WorkerReadyEvidence>) -> std::expected<void, Error>;
    [[nodiscard]] auto readiness_event() const noexcept -> const ReadinessEvent*;
    [[nodiscard]] static auto readiness_staging_bytes() noexcept -> std::size_t;
    auto finalization(std::uint64_t, const journal::Timestamp&, std::string_view)
        -> std::expected<void, Error>;
    [[nodiscard]] auto finalization_event() const noexcept -> const FinalizationEvent*;
    [[nodiscard]] static auto finalization_staging_bytes() noexcept -> std::size_t;
    auto terminal(std::uint64_t, const journal::Timestamp&,
        const runtime::PairwiseRunTerminalEvidence&) -> std::expected<void, Error>;
    [[nodiscard]] auto terminal_event() const noexcept -> const TerminalEvent*;
    [[nodiscard]] static constexpr auto terminal_staging_bytes() noexcept -> std::size_t {
        return sizeof(TerminalEvent) + 4U * sizeof(Error) + 4U * sizeof(std::size_t) +
            4U * sizeof(runtime::wire_v1::detail::TypedStockSum);
    }
    auto stopping(std::uint64_t, const journal::Timestamp&, std::string_view)
        -> std::expected<void, Error>;
    [[nodiscard]] auto stopping_events() const noexcept -> std::span<const StoppingEvent>;
    PairPrefix(PairPrefix&&) noexcept;
    auto operator=(PairPrefix&&) noexcept -> PairPrefix&;
    ~PairPrefix();
    PairPrefix(const PairPrefix&) = delete;
    auto operator=(const PairPrefix&) -> PairPrefix& = delete;
    auto open(std::uint64_t, const journal::Timestamp&, std::string_view)
        -> std::expected<void, Error>;
    auto factual(std::uint64_t, const journal::Timestamp&, std::string_view)
        -> std::expected<void, Error>;
    auto failed(std::uint64_t, const journal::Timestamp&, std::string_view)
        -> std::expected<void, Error>;
    auto close(std::uint64_t, const journal::Timestamp&, runtime::BlockAttemptCompletion)
        -> std::expected<void, Error>;
    auto reconcile(std::string_view) -> std::expected<void, Error>;
    // Consumes the prefix and compares the canonical root rebuilt from its
    // accepted components with the genuine producer's complete-root commitment.
    // The request-validated header is borrowed only for this call.
    auto reconcile_digest(const core::Sha256Digest&,
        const runtime::wire_v1::detail::ReconstructedEvidenceHeader&)
        -> std::expected<void, Error>;
    [[nodiscard]] auto events(std::size_t block) const noexcept -> std::span<const Event>;
    [[nodiscard]] auto block_count() const noexcept -> std::size_t;
    [[nodiscard]] auto consumed() const noexcept -> bool;
    [[nodiscard]] auto canonical_root() const noexcept -> std::string_view;
    [[nodiscard]] auto usage() const noexcept -> PairUsage;
    [[nodiscard]] auto domain() const noexcept -> std::optional<Domain>;
    // Private Results integration: a borrow cannot extend the context lifetime
    // or transfer its sealed admission capability.
    [[nodiscard]] auto lifecycle_context() const noexcept -> const ExpectedPairContext*;
    [[nodiscard]] auto lifecycle_outcomes_mode() const noexcept -> bool;

  private:
    explicit PairPrefix(std::unique_ptr<State>) noexcept;
    friend struct event_profile::detail::PairContextDecoder;
    auto monitoring_preparation_facts(std::uint64_t,const journal::Timestamp&,const MonitoringFactsView&)
        -> std::expected<void,Error>;
    auto preparation_association_facts(std::uint64_t,const journal::Timestamp&,const PreparationFactsLease&)
        -> std::expected<void,Error>;
    static auto prepare_decoded(PairFactsLease&&, PairLimits, runtime::StatisticsProductionPolicy,
        measurement::RepetitionPolicy, observation_timing::Limits) -> std::expected<PairPrefix,Error>;
    static auto prepare_impl(ExpectedPairContext&&, Domain, PairLimits,
        std::optional<runtime::StatisticsProductionPolicy>,
        std::optional<measurement::RepetitionPolicy>, bool terminal_mode = false, bool finalization_mode = false, bool readiness_mode = false, bool memory_mode = false, bool control_mode = false, bool monitoring_mode = false, bool preparation_mode = false, bool outcomes_mode = false) -> std::expected<PairPrefix, Error>;
    static auto prepare_impl(PairFactsLease&&, Domain, PairLimits,
        std::optional<runtime::StatisticsProductionPolicy>,
        std::optional<measurement::RepetitionPolicy>, bool terminal_mode = false, bool finalization_mode = false, bool readiness_mode = false, bool memory_mode = false, bool control_mode = false, bool monitoring_mode = false, bool preparation_mode = false, bool outcomes_mode = false) -> std::expected<PairPrefix, Error>;
    auto append(std::uint64_t, const journal::Timestamp&, std::string_view,
                runtime::wire_v1::EvidenceComponentKind, observation_timing::Input* = nullptr) -> std::expected<void, Error>;
    auto preparation_outcome(std::uint64_t, const journal::Timestamp&, std::string_view,
        runtime::wire_v1::EvidenceComponentKind) -> std::expected<void, Error>;
    std::unique_ptr<State> state_;
};
} // namespace epycbench::results::block_evidence
