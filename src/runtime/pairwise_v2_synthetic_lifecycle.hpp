#pragma once

#include "runtime/pairwise_v2_active_evidence.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace epycbench::runtime::detail {

struct SyntheticCaptureLimits;
class SyntheticLifecycleView;
enum class SyntheticCaptureFailure : unsigned char;

enum class SyntheticPhase : unsigned char {
    startup,
    execution,
    cancellation,
    completion,
    termination
};
enum class SyntheticStage : unsigned char {
    preparation,
    channels,
    spawn,
    parent_close,
    host_offer,
    capacity_offer,
    prepared_offer,
    request,
    host_ack,
    capacity_ack,
    prepared_ack,
    accepted,
    backend,
    encoding,
    manifest,
    chunks,
    completion,
    eof,
    reconstruction,
    results,
    process,
    cleanup
};
enum class SyntheticFailureKind : unsigned char {
    invalid_state,
    invalid_config,
    clock_overflow,
    clock_regression,
    callback,
    preparation,
    channels,
    spawn,
    close,
    protocol,
    backend,
    encoding,
    transport,
    reconstruction,
    results,
    cancelled,
    timeout,
    cancel_write,
    cancel_response,
    process_nonzero,
    process_signaled,
    terminate,
    kill,
    reap,
    wrong_child,
    unreapable
};
struct SyntheticFailure final {
    SyntheticFailureKind kind{};
    SyntheticStage stage{};
    SyntheticPhase phase{};
    std::optional<PairwiseV2ActiveEvidenceFailure> evidence;
};
struct SyntheticChild final {
    std::uint64_t token{};
    friend auto operator==(SyntheticChild, SyntheticChild) noexcept -> bool = default;
};
struct SyntheticExit final {
    SyntheticChild child;
    bool signaled{};
    std::uint32_t value{};
};
enum class SyntheticEndpoint : unsigned char {
    parent_runner_control,
    parent_runner_event,
    controller_control,
    controller_event,
    runner_event,
    runner_control
};
enum class SyntheticIo : unsigned char {
    control_write,
    control_read,
    event_write,
    event_read,
    cancel_write,
    cancel_read,
    cancelled_write,
    cancelled_read
};
enum class SyntheticOperationFailure : unsigned char { failed };

struct SyntheticLifecycleConfig final {
    // Fake monotonic ticks; all five durations are required, nonnegative, and
    // checked against the signed clock domain. Equality is expired.
    std::int64_t startup{}, execution{}, cancellation{}, completion{}, termination{};
};
struct SyntheticChannels final {
    ipc::ByteWriter* controller_control{};
    ipc::ByteReader* runner_control{};
    ipc::ByteWriter* runner_event{};
    ipc::ByteReader* controller_event{};
};
struct SyntheticProcessOperations final {
    // Borrowed fake dependencies and returned adapters must outlive this
    // lifecycle, including destructor retirement and the exact reap callback.
    void* context{};
    std::int64_t (*now)(void*){};
    std::optional<PairwiseV2CancellationReason> (*cancellation)(void*){};
    std::expected<SyntheticChannels, SyntheticOperationFailure> (*channels)(void*){};
    std::expected<SyntheticChild, SyntheticOperationFailure> (*spawn)(void*){};
    std::expected<void, SyntheticOperationFailure> (*close)(void*, SyntheticEndpoint){};
    // Readiness may advance fake time; it must never sleep or access a host.
    std::expected<bool, SyntheticOperationFailure> (*ready)(void*, SyntheticIo){};
    std::expected<std::optional<SyntheticExit>, SyntheticOperationFailure> (*try_wait)(
        void*, SyntheticChild){};
    std::expected<void, SyntheticOperationFailure> (*terminate)(void*, SyntheticChild){};
    std::expected<void, SyntheticOperationFailure> (*kill)(void*, SyntheticChild){};
    std::expected<SyntheticExit, SyntheticOperationFailure> (*reap)(void*, SyntheticChild){};
    // Mandatory fatal-invariant observer: failed exact reap cannot silently
    // release child ownership or become success, even when this fake returns.
    void (*unreapable)(void*, SyntheticChild) noexcept {};
};
struct SyntheticBackendPreparation final {
    // The returned backend's context owns its prepared ledger and must outlive
    // the lifecycle; only the admitted view passed to prepare is call-scoped.
    void* context{};
    // Allocating fixture preparation only, before acknowledgements. The view
    // is borrowed for this call only; the returned backend moves owned typed
    // evidence after activation, and must not retain this admitted view.
    std::expected<PairwiseV2FakeEvidenceBackend, SyntheticOperationFailure> (*prepare)(
        void*, const PairwiseV2AdmittedStartup&){};
};
enum class SyntheticCleanupSource : unsigned char {
    backend_retirement,
    runner_control,
    runner_event,
    controller_control,
    controller_event,
    graceful_termination,
    forced_escalation,
    exact_reap
};
struct SyntheticCleanupFailure final {
    SyntheticCleanupSource source{};
    SyntheticFailure failure;
};
struct SyntheticLifecycleOutcome final {
    std::optional<SyntheticFailure> primary;
    std::array<SyntheticCleanupFailure, 8U> secondary{};
    std::size_t secondary_count{};
    // Factual observations are not additional diagnostic ledger entries.
    std::uint8_t timeout_phases{};
    bool clock_failed{}, callback_failed{}, cancel_write_failed{}, cancel_response_failed{};
    std::uint8_t parent_close_failures{};
    std::optional<results::PairwiseRunResult> result;
    std::optional<SyntheticExit> observed_exit;
    std::optional<SyntheticExit> exit;
    std::optional<PairwiseV2CancellationReason> cancellation;
    bool request_complete{}, accepted{}, activated{}, success_started{}, completion_received{},
        clean_eof{};
    bool cancel_sent{}, runner_observed_cancel{}, cancelled_received{};
    bool terminate_attempted{}, kill_attempted{}, exact_reap{}, fatal_unreapable{};
    [[nodiscard]] auto succeeded() const noexcept -> bool {
        return !primary && result && exact_reap && clean_eof && exit && !exit->signaled &&
               exit->value == 0U;
    }
};

class PairwiseV2SyntheticLifecycle final {
  public:
    PairwiseV2SyntheticLifecycle(PairwiseV2SyntheticLifecycle&&) noexcept;
    PairwiseV2SyntheticLifecycle(const PairwiseV2SyntheticLifecycle&) = delete;
    auto operator=(PairwiseV2SyntheticLifecycle&&) -> PairwiseV2SyntheticLifecycle& = delete;
    ~PairwiseV2SyntheticLifecycle() noexcept;
    [[nodiscard]] static auto
    prepare(wire_v1::detail::StartupSessionId, const PairwiseHostCapacityRequestTranscript&,
            const RunnerContext& controller_context, RunnerContext runner_context,
            PairwiseMonitoringHostCapacityStartupAccess::LowerOperations controller_authority,
            PairwiseMonitoringHostCapacityStartupAccess::LowerOperations runner_authority,
            SyntheticLifecycleConfig, SyntheticProcessOperations,
            SyntheticBackendPreparation, const SyntheticCaptureLimits* capture = nullptr) noexcept
        -> std::expected<PairwiseV2SyntheticLifecycle, SyntheticFailure>;
    // Runs real allocating startup composition. Successful return is precisely
    // the boundary before which the test enables global allocation denial.
    [[nodiscard]] auto start() noexcept -> std::expected<void, SyntheticFailure>;
    // On startup failure this retrieves its already retired fixed outcome.
    // Otherwise it executes backend/evidence and all process retirement work.
    [[nodiscard]] auto finish() noexcept -> SyntheticLifecycleOutcome;
    [[nodiscard]] auto capture_retired() noexcept
        -> std::expected<SyntheticLifecycleView, SyntheticCaptureFailure>;

  private:
    struct Storage;
    explicit PairwiseV2SyntheticLifecycle(std::unique_ptr<Storage>) noexcept;
    std::unique_ptr<Storage> storage_;
};

} // namespace epycbench::runtime::detail
