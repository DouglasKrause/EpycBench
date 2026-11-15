#pragma once

#include "epycbench/measurement/observation.hpp"
#include "epycbench/measurement/validity.hpp"
#include "epycbench/runtime/production_admission.hpp"
#include "runtime/pairwise_memory_owning_facade.hpp"
#include "runtime/pairwise_monitoring_owning_facade.hpp"
#include "runtime/pairwise_monitoring_preparation_facts.hpp"
#include "runtime/pairwise_worker_owning_facade.hpp"

#include <expected>
#include <memory>
#include <span>
#include <variant>

namespace epycbench::runtime::detail {

enum class FrozenObservationRunReason : unsigned char {
    invalid_input,
    capacity,
    allocation,
    contained_exception,
    unavailable,
    wrong_family,
    ordinal,
    cancelled,
    operational_failure,
};
using FrozenObservationWork =
    std::variant<kernel::SequentialReadBandwidthPassCount, kernel::DependentLoadLatencyStepCount>;

struct FrozenObservationRunDependencies final {
    PairwisePlatformLeafFacade platform;
    PairwiseKernelLeafFacade kernel;
    PairwiseWorkerOwningDependencies workers;
    PairwiseMemoryOwningHooks memory_hooks{};
    BoundedMonitoringStorageDependencies sources{};
    // Production source preparation derives discovery dimensions from the
    // genuine per-Run capacity. Explicit injected dimensions remain the default.
    bool source_capacity_from_admission{};
};

// Complete checked stock, including cursor-only sysfs/service failures. This is
// preparation inventory, not an observation, producer or qualification authority.
struct FrozenObservationFailureStock final {
    std::size_t memory{};
    std::size_t worker{};
    std::size_t monitoring_preparation{};
    std::size_t monitoring_capture{};
    std::size_t monitoring_stop{};
    std::size_t coordination{};
    std::size_t total{};
};
[[nodiscard]] auto frozen_observation_failure_stock(std::uint64_t workers, std::uint64_t domains,
                                                    std::uint64_t rollback_values,
                                                    std::uint64_t completion_values,
                                                    std::uint64_t stop_values, bool temperature,
                                                    bool frequency) noexcept
    -> std::optional<FrozenObservationFailureStock>;

struct FrozenObservation final {
    core::ObservationId observation_id;
    core::ExperimentId experiment_id;
    kernel::DependentLoadLatencyBlockId block_id;
    measurement::MeasurementObservation observation;
    measurement::ObservationValidityEvidence validity;
    monitoring::MonitoringCaptureResult monitoring;
    std::vector<measurement::WorkerPlacementValidityEvidence> placements;
    std::vector<measurement::WorkerMigrationValidityEvidence> migrations;
};

struct FrozenObservationWorkerFinalization final {
    core::WorkerId worker_id;
    bool timed_entered{};
    std::optional<memory::RegionFinalizationDiagnostic> diagnostic;
    std::optional<memory::RegionCleanupEvidence> partial_cleanup;
    bool ownership_remains{};
};

struct FrozenObservationTerminalStock final {
    std::size_t workers{}, forced_worker_errors{};
};
[[nodiscard]] auto frozen_observation_terminal_stock(std::uint64_t workers) noexcept
    -> std::optional<FrozenObservationTerminalStock>;
struct FrozenObservationWorkerAssociation final {
    core::WorkerId worker_id;
    core::LinuxCpuId cpu;
};
enum class FrozenObservationMetadataComparison : unsigned char {
    not_requested, no_completed_capture, matched, mismatch,
};
struct FrozenObservationPreparationFacts final {
    MonitoringPreparationFactDomain domain;
    std::size_t workers{}, frequency_domains{};
    std::array<MonitoringPreparationChannelFact, 4> channels;
    std::optional<MonitoringPreparationServiceFact> service;
    std::vector<FrozenObservationWorkerAssociation> worker_associations;
};
struct FrozenObservationTerminalFacts final {
    std::vector<PairwiseEvidenceFailure> failures;
    std::vector<FrozenObservationWorkerFinalization> workers;
    std::optional<FrozenObservationPreparationFacts> preparation;
    FrozenObservationMetadataComparison metadata;
    std::array<PairwiseMonitoringForceDisposition, 3> monitoring_force;
    std::size_t monitoring_force_count{};
    std::vector<ForcedTeamStopError> worker_force;
};

struct FrozenObservationRunState;
class FrozenObservationTerminalStockTestAccess;
class FrozenObservationRetirement final {
  public:
    FrozenObservationRetirement(FrozenObservationRetirement&&) noexcept;
    FrozenObservationRetirement(const FrozenObservationRetirement&) = delete;
    auto operator=(FrozenObservationRetirement&&) -> FrozenObservationRetirement& = delete;
    ~FrozenObservationRetirement() noexcept;
    [[nodiscard]] auto failures() const noexcept -> std::span<const PairwiseEvidenceFailure>;
    [[nodiscard]] auto workers() const noexcept
        -> std::span<const FrozenObservationWorkerFinalization>;
    [[nodiscard]] auto cleanup_confirmed() const noexcept -> bool;
    [[nodiscard]] auto monitoring_force_dispositions() const noexcept
        -> std::span<const PairwiseMonitoringForceDisposition>;
    [[nodiscard]] auto worker_force_errors() const noexcept -> std::span<const ForcedTeamStopError>;
    [[nodiscard]] auto preparation_facts() const noexcept
        -> std::optional<MonitoringPreparationFactView>;
    // Refusal leaves this actual terminal owner intact. Confirmed transfer moves
    // owning evidence and destroys native State before returning, without allocation.
    [[nodiscard]] auto detach_confirmed(
        const monitoring::MonitoringCaptureResult* completed_capture = nullptr) && noexcept
        -> std::expected<FrozenObservationTerminalFacts, FrozenObservationRunReason>;

  private:
    explicit FrozenObservationRetirement(std::unique_ptr<FrozenObservationRunState>) noexcept;
    std::unique_ptr<FrozenObservationRunState> state_;
    friend class FrozenObservationRun;
    friend class FrozenObservationTerminalStockTestAccess;
};

// Source-private negative test seam on an already confirmed terminal owner.
// It varies stock or consumes an actual identity-matched negative command outcome;
// neither operation prepares/activates a Run or establishes producer authority.
class FrozenObservationTerminalStockTestAccess final {
  public:
    static auto replace_confirmed_stock(FrozenObservationRetirement&,
        std::vector<FrozenObservationWorkerAssociation>&&,
        std::vector<ForcedTeamStopError>&&) noexcept -> bool;
    // Negative boundary composition only: exact actual WorkerIds, confirmed
    // retirement and sufficient original fixed failure stock are required.
    static auto consume_confirmed_command_failures(FrozenObservationRetirement&,
        const PairwiseWorkerCommandOutcome&) noexcept -> bool;
};

// Source-private and unselected. It borrows actual admitted inputs and dependency
// contexts; neither this type nor its outputs establish H3.1 profile membership,
// predecessor qualification, controller placement or campaign authorization.
class FrozenObservationRun final {
  public:
    FrozenObservationRun(FrozenObservationRun&&) noexcept;
    FrozenObservationRun(const FrozenObservationRun&) = delete;
    auto operator=(FrozenObservationRun&&) -> FrozenObservationRun& = delete;
    ~FrozenObservationRun() noexcept;

    [[nodiscard]] static auto
    prepare(const AdmittedPlacementRequest&, const monitoring::MonitoringExecutionPolicy&,
            const ServicePlacementPlan&, PairwiseAdmittedMonitoringCapacity, FrozenObservationWork,
            std::size_t observation_capacity, measurement::TimerOverheadEstimate,
            RunnerCancellation&, FrozenObservationRunDependencies) noexcept
        -> std::expected<FrozenObservationRun, FrozenObservationRunReason>;
    [[nodiscard]] auto activate() noexcept -> std::expected<void, FrozenObservationRunReason>;
    [[nodiscard]] auto observe_bandwidth(std::size_t ordinal, core::ObservationId,
                                         core::ExperimentId,
                                         kernel::DependentLoadLatencyBlockId) noexcept
        -> std::expected<FrozenObservation, FrozenObservationRunReason>;
    [[nodiscard]] auto observe_latency(std::size_t ordinal, core::ObservationId, core::ExperimentId,
                                       kernel::DependentLoadLatencyBlockId) noexcept
        -> std::expected<FrozenObservation, FrozenObservationRunReason>;
    [[nodiscard]] auto finish() && noexcept
        -> std::expected<FrozenObservationRetirement, FrozenObservationRunReason>;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto failures() const noexcept -> std::span<const PairwiseEvidenceFailure>;
    [[nodiscard]] auto failure_capacity() const noexcept -> std::size_t;

  private:
    explicit FrozenObservationRun(std::unique_ptr<FrozenObservationRunState>) noexcept;
    [[nodiscard]] auto observe(std::size_t, core::ObservationId, core::ExperimentId,
                               kernel::DependentLoadLatencyBlockId, bool bandwidth) noexcept
        -> std::expected<FrozenObservation, FrozenObservationRunReason>;
    std::unique_ptr<FrozenObservationRunState> state_;
};

} // namespace epycbench::runtime::detail
