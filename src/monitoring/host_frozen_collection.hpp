#pragma once

#include "monitoring/host_frozen_observation_admission.hpp"
#include "platform_linux/host_work_admission.hpp"
#include "runtime/frozen_observation_run.hpp"
#include "epycbench/measurement/block_scheduler.hpp"
#include "epycbench/measurement/empty_harness.hpp"
#include "epycbench/statistics/block_representatives.hpp"

namespace epycbench::monitoring::qualification::detail {

enum class HostFrozenPhase : unsigned char { q5, q6 };
enum class HostFrozenDomain : unsigned char { source, synthetic };
enum class HostFrozenCollectionReason : unsigned char {
    invalid_input, unavailable, allocation, contained_exception, admission, capacity,
    headroom, clock, deadline, cancelled, activation, observation, retirement,
    metadata, invalid_observation, statistics, outstanding, association, abandoned, output,
};
struct HostFrozenSourceInputs final {
    const RunnerContextAcquisitionSession& session;
    const MonitoringHostQualificationPlan& plan;
    const HostQualificationCalibrationInputs& calibration;
    const MonitoringQ1ValidatedPublicValues& public_values;
    runtime::ProductionRequestIdSource& request_ids;
    const class FrozenSourceControlCapability* control{};
    const HostEnvironmentAuthority* environment{};
    std::optional<core::Error>* raw_failure{};
};
struct HostFrozenCapacityOperations final {
    void* context{};
    std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                  platform_linux::detail::LinuxMonitoringCapacityFailure>
        (*mint)(void*, const runtime::AdmittedPlacementRequest&,
                runtime::PairwiseMonitoringCapacityAcquisitionLeg) noexcept{};
};
[[nodiscard]] auto production_host_frozen_capacity_operations() noexcept
    -> HostFrozenCapacityOperations;
struct HostFrozenCollectionDependencies final {
    runtime::detail::FrozenObservationRunDependencies run;
    HostFrozenCapacityOperations capacity;
    platform_linux::host_work::ReadOperations headroom;
    platform_linux::host_work::GuardOperations guard;
    measurement::detail::RawMonotonicClockOperations empty_clock;
};
struct HostFrozenCellIdentity final {
    HostFrozenPhase phase;
    HostQ0bFamily family;
    HostFrozenConfiguration configuration;
    std::size_t ordinal{}, comparison{}, block{}, slot{};
    measurement::ComparisonVariantSlot treatment;
};
using HostFrozenBlockRepresentatives = std::variant<
    statistics::BlockRepresentatives<statistics::Bandwidth>,
    statistics::BlockRepresentatives<statistics::DependentLoadLatency>>;
struct HostFrozenCollectionSnapshot final {
    HostFrozenPhase phase;
    HostFrozenDomain domain;
    std::size_t consumed{}, expected{}, representatives{}, pairs{};
    bool outstanding{}, terminal{}, completed{};
    std::optional<HostFrozenCollectionReason> failure;
};
// Conservative project-owned Run stock, derived from genuine admitted inputs.
// Untimed dependency allocator internals remain within the approved reserve.
[[nodiscard]] auto host_frozen_run_resources(const runtime::AdmittedPlacementRequest&,
    const runtime::detail::PairwiseAdmittedMonitoringCapacity&) noexcept
    -> std::expected<platform_linux::host_work::Resources, HostFrozenCollectionReason>;
struct HostFrozenCollectionState;
struct HostFrozenCellState;
struct HostFrozenFailureState;
class HostFrozenCollectionTestAccess;
class HostFrozenCell final {
  public:
    HostFrozenCell(HostFrozenCell&&) noexcept;
    HostFrozenCell(const HostFrozenCell&) = delete;
    auto operator=(HostFrozenCell&&) -> HostFrozenCell& = delete;
    ~HostFrozenCell() noexcept;
    [[nodiscard]] auto identity() const noexcept -> std::optional<HostFrozenCellIdentity>;
    [[nodiscard]] auto domain() const noexcept -> HostFrozenDomain;
    [[nodiscard]] auto admission() const noexcept -> const HostFrozenObservationAdmission*;
    [[nodiscard]] auto placement() const noexcept -> const runtime::AdmittedPlacementRequest*;
    [[nodiscard]] auto observation() const noexcept -> const runtime::detail::FrozenObservation*;
    [[nodiscard]] auto terminal_facts() const noexcept
        -> const runtime::detail::FrozenObservationTerminalFacts*;
    [[nodiscard]] auto empty_trials() const noexcept -> std::span<const measurement::EmptyHarnessTrial>;
    [[nodiscard]] auto timer_estimate() const noexcept -> std::optional<measurement::TimerOverheadEstimate>;
    [[nodiscard]] auto representatives() const noexcept -> const HostFrozenBlockRepresentatives*;
  private:
    explicit HostFrozenCell(std::unique_ptr<HostFrozenCellState>) noexcept;
    std::unique_ptr<HostFrozenCellState> state_;
    friend class HostFrozenCollection;
};
class HostFrozenCollectionFailure final {
  public:
    HostFrozenCollectionFailure(HostFrozenCollectionFailure&&) noexcept;
    HostFrozenCollectionFailure(const HostFrozenCollectionFailure&) = delete;
    auto operator=(HostFrozenCollectionFailure&&) -> HostFrozenCollectionFailure& = delete;
    ~HostFrozenCollectionFailure() noexcept;
    [[nodiscard]] auto reason() const noexcept -> HostFrozenCollectionReason;
    [[nodiscard]] auto terminal_facts() const noexcept
        -> const runtime::detail::FrozenObservationTerminalFacts*;
    [[nodiscard]] auto retained_retirement() const noexcept
        -> const runtime::detail::FrozenObservationRetirement*;
    [[nodiscard]] auto capacity_failure() const noexcept
        -> const platform_linux::detail::LinuxMonitoringCapacityFailure*;
    [[nodiscard]] auto headroom_failure() const noexcept -> const platform_linux::host_work::Failure*;
    [[nodiscard]] auto admission_reason() const noexcept -> std::optional<HostFrozenAdmissionReason>;
    [[nodiscard]] auto binding_checkpoint() const noexcept -> HostFrozenBindingCheckpoint;
    [[nodiscard]] auto lower_error() const noexcept -> const core::Error*;
    [[nodiscard]] auto observation() const noexcept -> const runtime::detail::FrozenObservation*;
    [[nodiscard]] auto admission() const noexcept -> const HostFrozenObservationAdmission*;
    [[nodiscard]] auto binding_failure() const noexcept
        -> std::optional<runtime::detail::PairwiseMonitoringHostCapacityAdmissionFailure>;
    [[nodiscard]] auto projection_failure() const noexcept -> const runtime::PairwiseRuntimeFailure*;
    [[nodiscard]] auto charge_retained() const noexcept -> bool;
  private:
    explicit HostFrozenCollectionFailure(HostFrozenCollectionReason) noexcept;
    explicit HostFrozenCollectionFailure(std::unique_ptr<HostFrozenFailureState>) noexcept;
    HostFrozenCollectionReason reason_;
    std::unique_ptr<HostFrozenFailureState> state_;
    friend class HostFrozenCollection;
};
class HostFrozenCollection final {
  public:
    HostFrozenCollection(HostFrozenCollection&&) noexcept;
    HostFrozenCollection(const HostFrozenCollection&) = delete;
    auto operator=(HostFrozenCollection&&) -> HostFrozenCollection& = delete;
    ~HostFrozenCollection() noexcept;
    [[nodiscard]] static auto prepare_q5_source(HostFrozenSourceInputs,
        platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&,
        HostFrozenCollectionDependencies) noexcept
        -> std::expected<HostFrozenCollection, HostFrozenCollectionFailure>;
    [[nodiscard]] static auto prepare_q6_source(HostFrozenSourceInputs,
        platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&,
        HostFrozenCollectionDependencies) noexcept
        -> std::expected<HostFrozenCollection, HostFrozenCollectionFailure>;
    [[nodiscard]] auto next() noexcept
        -> std::expected<std::optional<HostFrozenCell>, HostFrozenCollectionFailure>;
    [[nodiscard]] auto consume_cell(HostFrozenCell&&) noexcept
        -> std::expected<void, HostFrozenCollectionReason>;
    void fail_output() noexcept;
    [[nodiscard]] auto owns_failure(const HostFrozenCollectionFailure&) const noexcept -> bool;
    [[nodiscard]] auto snapshot() const noexcept -> std::optional<HostFrozenCollectionSnapshot>;
  private:
    // Explicit synthetic seam holds actual tiny admitted endpoints, never Runs.
    struct Endpoint final {
        const runtime::AdmittedPlacementRequest* placement{};
        const MonitoringExecutionPolicy* policy{};
        const runtime::ServicePlacementPlan* service{};
        HostFrozenWork work;
    };
    static auto prepare(HostFrozenPhase, std::optional<HostFrozenSourceInputs>,
        std::span<const Endpoint>, platform_linux::host_work::ResourceLedger&,
        runtime::RunnerCancellation&, HostFrozenCollectionDependencies,
        std::optional<HostFrozenSourceInputs> owned_synthetic = std::nullopt) noexcept
        -> std::expected<HostFrozenCollection, HostFrozenCollectionFailure>;
    // Same owner/deadline/revalidation gates with explicitly synthetic work.
    // Only the existing Control owner can select this private fixture path.
    static auto prepare_owned_synthetic(HostFrozenPhase, std::span<const Endpoint>,
        HostFrozenSourceInputs, platform_linux::host_work::ResourceLedger&,
        runtime::RunnerCancellation&, HostFrozenCollectionDependencies) noexcept
        -> std::expected<HostFrozenCollection, HostFrozenCollectionFailure>;
    // Pure failure-transport seam; no Collection, Control capability or Run is
    // constructed. It uses the same scalar/lower-owner retention as both callers.
    static auto retain_admission_failure_for_testing(HostFrozenAdmissionFailure&&)
        -> HostFrozenCollectionFailure;
    explicit HostFrozenCollection(std::shared_ptr<HostFrozenCollectionState>) noexcept;
    std::shared_ptr<HostFrozenCollectionState> state_;
    friend class HostFrozenCollectionTestAccess;
    friend struct HostFrozenCollectionState;
    friend class FrozenSourceControlOwner;
    friend struct FrozenSourceControlState;
};
#if defined(EPYCBENCH_HOST_FROZEN_COLLECTION_TEST)
class HostFrozenCollectionTestAccess final {
  public:
    using Endpoint = HostFrozenCollection::Endpoint;
    static auto retain_admission_failure(HostFrozenAdmissionFailure&& failure) {
        return HostFrozenCollection::retain_admission_failure_for_testing(std::move(failure));
    }
    static auto prepare_synthetic(HostFrozenPhase phase, std::span<const Endpoint> endpoints,
        platform_linux::host_work::ResourceLedger& ledger, runtime::RunnerCancellation& cancel,
        HostFrozenCollectionDependencies dependencies) noexcept {
        return HostFrozenCollection::prepare(phase, std::nullopt, endpoints, ledger, cancel,
                                             std::move(dependencies));
    }
};
#endif
} // namespace epycbench::monitoring::qualification::detail
