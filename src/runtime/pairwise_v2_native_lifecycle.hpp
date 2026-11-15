#pragma once

#include "runtime/controller_lifecycle_access.hpp"
#include "runtime/pairwise_v2_active_evidence.hpp"
#include "runtime/pairwise_prepared_runner_execution.hpp"
#include "runtime/pairwise_v2_production_control_monitor.hpp"
#include "platform_linux/current_process_image.hpp"

#include <array>
#include <memory>

namespace epycbench::runtime::detail {

enum class NativePairwisePhase : unsigned char { startup, execution, cancellation, completion, termination };
enum class NativePairwiseStage : unsigned char {
    preparation, channels, spawn, parent_close, startup_control, startup_events,
    cancellation, backend, encoding, evidence, eof, reconstruction, results,
    process, endpoint_close, terminate, kill, reap
};
enum class NativePairwiseFailureKind : unsigned char {
    invalid_state, invalid_input, capacity, preparation, clock_overflow, clock_regression,
    native, protocol, backend, encoding, transport, reconstruction, results,
    cancelled, timeout, process_nonzero, process_signaled, unreapable
};
struct NativePairwiseFailure final {
    NativePairwiseFailureKind kind{};
    NativePairwiseStage stage{};
    NativePairwisePhase phase{};
    std::optional<platform_linux::detail::FixedProcessFailure> native;
    std::optional<PairwiseV2ComposedStartupFailure> startup{};
};

// Fixed local startup budget. Created at hidden-entry arrival and retained
// across context construction, admission, preparation and every ack write.
class NativeRunnerStartupBudget final {
  public:
    [[nodiscard]] static auto start(ControllerClock, std::chrono::milliseconds,
        std::chrono::milliseconds) noexcept
        -> std::expected<NativeRunnerStartupBudget, NativePairwiseFailure>;
    [[nodiscard]] auto point() noexcept -> bool;
    [[nodiscard]] auto poll_timeout() noexcept -> int;
    [[nodiscard]] auto failure() const noexcept -> const std::optional<NativePairwiseFailure>& {
        return failure_;
    }
  private:
    NativeRunnerStartupBudget(ControllerClock clock, ControllerTimePoint now,
        ControllerTimePoint deadline, ControllerTimePoint::duration quantum) noexcept
        : clock_{clock}, last_{now}, deadline_{deadline}, quantum_{quantum} {}
    ControllerClock clock_;
    ControllerTimePoint last_, deadline_;
    ControllerTimePoint::duration quantum_;
    std::optional<NativePairwiseFailure> failure_;
};
struct NativePairwiseOutcome final {
    std::optional<NativePairwiseFailure> primary;
    std::array<NativePairwiseFailure, 8> cleanup{};
    std::size_t cleanup_count{};
    std::uint8_t timeout_phases{};
    std::optional<platform_linux::ProcessExitStatus> exit;
    std::optional<PairwiseV2CancellationReason> cancellation;
    std::optional<PairwiseV2CancellationPhase> runner_observation;
    std::optional<results::PairwiseRunResult> result;
    std::uint64_t progress_received{}, progress_acknowledged{};
    std::optional<progress::Acknowledgement> last_journal_receipt;
    bool request_complete{}, accepted{}, activated{}, success_started{}, completion_received{},
        clean_eof{}, cancel_sent{}, cancelled_received{}, terminate_attempted{}, kill_attempted{},
        exact_reap{}, unreapable{}, closed_with_error{};
    [[nodiscard]] auto succeeded() const noexcept -> bool {
        return !primary && result && clean_eof && exact_reap && exit &&
            std::holds_alternative<platform_linux::ProcessExited>(*exit) &&
            std::get<platform_linux::ProcessExited>(*exit).code == 0;
    }
};
enum class NativePairwiseRunnerMode : unsigned char { cooperative, prepared_production };
enum class NativeLifecycleKind : unsigned char {
    spawned, spawn_failed, request_sent, accepted, completion, cancelled, eof,
    reaped, endpoint_closed, termination_sent, kill_sent, failure, retired
};
enum class NativeLifecycleEndpoint : unsigned char {
    parent_runner_control, parent_runner_event, controller_control, controller_event
};
struct NativeLifecycleObservation final {
    NativeLifecycleKind kind{};
    NativePairwiseStage stage{};
    NativePairwisePhase phase{};
    std::optional<NativePairwiseFailure> failure;
    std::optional<platform_linux::ProcessExitStatus> exit;
    std::optional<PairwiseV2CancellationReason> cancellation;
    std::optional<PairwiseV2CancellationPhase> runner_observation;
    bool succeeded{};
    std::optional<NativeLifecycleEndpoint> endpoint;
    // Selected Runtime outcome disposition; later unselected observations do
    // not silently become cleanup failures in the journal.
    bool secondary_failure{}, unselected_failure{};
    bool cancel_sent{}, cancelled_received{};
};
struct NativeLifecycleObserver final {
    void* context{};
    // Fixed, nonallocating capture only. Called in actual observation order;
    // context outlives controller destruction and cleanup. No PID/handle escapes.
    bool (*capture)(void*, const NativeLifecycleObservation&) noexcept{};
    // Ordinary journal window: child exists, but no startup bytes were sent.
    // This callback cannot authorize continuation after the original deadline.
    bool (*before_startup)(void*) noexcept{};
};
struct ValidatedProgressView final {
    const progress::Offer& offer;
    const progress::Transition& transition;
};
struct ProgressConsumeResult final {
    enum class State : unsigned char { continue_run, stop, failed } state{State::failed};
    progress::Acknowledgement receipt;
};
struct NativeProgressConsumer final {
    progress::Configuration configuration;
    void* context{};
    // Results owns this callback and all journal/semantic state. Its lifetime
    // contains the controller; invocation occurs only for a validated component.
    ProgressConsumeResult (*consume_progress)(void*, const ValidatedProgressView&) noexcept{};
};
struct NativePairwiseDependencies final {
    ControllerChannelFactory channels{};
    platform_linux::detail::CurrentProcessImage* image{};
    std::span<const std::string_view> arguments;
    ControllerClock clock{};
    CancellationObserver cancellation{};
    ControllerFatalInvariant fatal{};
    NativePairwiseRunnerMode runner_mode{NativePairwiseRunnerMode::cooperative};
    std::optional<NativeProgressConsumer> progress{};
    stage_b_policy::Selection policy{stage_b_policy::Selection::existing_caller};
    PairwiseV2AuthoritySource authority_source{PairwiseV2AuthoritySource::supplied_operations};
    NativeLifecycleObserver lifecycle{};
};
// New auxiliary bytes only; accepted lower prepared capacities are separate.
struct NativePairwiseLimits final { std::size_t auxiliary_bytes{}; };

class PairwiseV2NativeController final {
  public:
    PairwiseV2NativeController(PairwiseV2NativeController&&) noexcept;
    PairwiseV2NativeController(const PairwiseV2NativeController&) = delete;
    auto operator=(PairwiseV2NativeController&&) -> PairwiseV2NativeController& = delete;
    ~PairwiseV2NativeController() noexcept;
    [[nodiscard]] static auto auxiliary_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto prepare(wire_v1::detail::StartupSessionId,
        const PairwiseHostCapacityRequestTranscript&, const RunnerContext&,
        PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        ControllerLifecycleConfig, NativePairwiseDependencies, NativePairwiseLimits) noexcept
        -> std::expected<PairwiseV2NativeController, NativePairwiseFailure>;
    [[nodiscard]] auto start() noexcept -> std::expected<void, NativePairwiseFailure>;
    // Preparation-only factual borrows, invalidated by start/move/destruction.
    // Neither getter asserts that the runner has acknowledged the offer.
    [[nodiscard]] auto prepared_capacity_offer() const noexcept
        -> const wire_v1::detail::AuthoredCapacityOffer*;
    [[nodiscard]] auto progress_before_ack() const noexcept -> const progress::Offer*;
    // Selected Stage-B preparation derives the offer locally before the Results
    // consumer exists. Bind that consumer once, while still unable to spawn.
    [[nodiscard]] auto bind_progress_consumer(NativeProgressConsumer) noexcept
        -> std::expected<void,NativePairwiseFailure>;
    [[nodiscard]] auto finish() noexcept -> NativePairwiseOutcome;
    // One-use borrow after completed receiver EOF and lifecycle retirement. The
    // controller must remain unmoved and alive for the entire borrow.
    [[nodiscard]] auto take_validated_bytes() noexcept
        -> std::expected<std::span<const std::byte>, NativePairwiseFailure>;
  private:
    struct Storage;
    explicit PairwiseV2NativeController(std::unique_ptr<Storage>) noexcept;
    std::unique_ptr<Storage> storage_;
};

enum class NativeBackendStep : unsigned char { pending, ready, cancelled, failed };
struct NativeCooperativeBackend final {
    // Prepared owner and callbacks survive the runner. No actual production backend selection.
    std::shared_ptr<void> owner;
    void* context{};
    NativeBackendStep (*step)(void*, std::optional<PairwiseV2CancellationReason>) noexcept{};
    bool (*retire)(void*) noexcept{};
    PairwiseV2FakeEvidenceBackend evidence{};
};
struct NativeBackendPreparation final {
    void* context{};
    std::expected<NativeCooperativeBackend, PairwiseV2FakeBackendFailure> (*prepare)(
        void*, const PairwiseV2AdmittedStartup&){};
};
struct NativePreparedProductionDependencies final {
    PairwiseTypedProductionBindingDependencies typed;
    ServicePlatformOperations monitor;
    std::optional<progress::Configuration> progress{};
    std::optional<PreparationGraphLimits> graph_limits{};
    PairwiseV2AuthoritySource authority_source{PairwiseV2AuthoritySource::supplied_operations};
};
struct NativePreparedPairwiseOutcome final {
    NativePairwiseOutcome lifecycle;
    std::optional<PreparedPairwiseRunnerExecution> execution;
    std::optional<PairwiseV2ActiveEvidenceRunner> evidence;
    std::optional<PairwiseV2ProductionControlMonitor> control;
};
class PairwiseV2NativeRunner final {
  public:
    PairwiseV2NativeRunner(PairwiseV2NativeRunner&&) noexcept;
    PairwiseV2NativeRunner(const PairwiseV2NativeRunner&) = delete;
    auto operator=(PairwiseV2NativeRunner&&) -> PairwiseV2NativeRunner& = delete;
    ~PairwiseV2NativeRunner() noexcept;
    [[nodiscard]] static auto auxiliary_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto prepare(platform_linux::RunnerIpcEndpoints,
        wire_v1::detail::StartupSessionId, ipc::RequestId, RunnerContext,
        PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        NativeBackendPreparation, NativePairwiseLimits) noexcept
        -> std::expected<PairwiseV2NativeRunner, NativePairwiseFailure>;
    [[nodiscard]] auto run() noexcept -> NativePairwiseOutcome;
    [[nodiscard]] static auto production_auxiliary_bytes(bool progress = false) noexcept -> std::size_t;
    [[nodiscard]] static auto prepare_production(platform_linux::RunnerIpcEndpoints,
        wire_v1::detail::StartupSessionId, ipc::RequestId, RunnerContext,
        PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        NativePreparedProductionDependencies, NativePairwiseLimits) noexcept
        -> std::expected<PairwiseV2NativeRunner, NativePairwiseFailure>;
    [[nodiscard]] static auto prepare_production_from_channel(platform_linux::RunnerIpcEndpoints,
        RunnerContext, PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        NativePreparedProductionDependencies, NativePairwiseLimits,
        NativeRunnerStartupBudget) noexcept
        -> std::expected<PairwiseV2NativeRunner, NativePairwiseFailure>;
    [[nodiscard]] auto run_production() noexcept -> NativePreparedPairwiseOutcome;
  private:
    [[nodiscard]] static auto prepare_production_impl(platform_linux::RunnerIpcEndpoints,
        std::optional<wire_v1::detail::StartupSessionId>, std::optional<ipc::RequestId>, RunnerContext,
        PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        NativePreparedProductionDependencies, NativePairwiseLimits,
        std::optional<NativeRunnerStartupBudget>) noexcept
        -> std::expected<PairwiseV2NativeRunner, NativePairwiseFailure>;
    struct Storage;
    explicit PairwiseV2NativeRunner(std::unique_ptr<Storage>) noexcept;
    std::unique_ptr<Storage> storage_;
};

} // namespace epycbench::runtime::detail
