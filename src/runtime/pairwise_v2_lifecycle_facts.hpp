#pragma once

#include "runtime/pairwise_v2_synthetic_lifecycle.hpp"

#include <type_traits>
#include <utility>

namespace epycbench::runtime::detail {

// One fixed cell per source transition; no per-poll or per-frame trace.
enum class SyntheticLifecycleMark : unsigned char {
    channels_ready, child_created, request_written, acknowledgments_validated,
    active_roles_bound, backend_entered, backend_returned_evidence, canonical_encoded,
    first_success_stream_byte, sender_completed, completion_received, clean_eof,
    typed_reconstruction_completed, results_projected, cancellation_decided,
    cancel_written, runner_observed_cancel, cancelled_frame_written,
    cancelled_frame_validated, retirement_begun, retirement_completed,
    fatal_unreapable_observed,
    startup_begin, startup_expiry, execution_begin, execution_expiry,
    cancellation_begin, cancellation_expiry, completion_begin, completion_expiry,
    termination_begin, termination_expiry,
    parent_runner_control_close_attempt, parent_runner_control_close_return,
    parent_runner_event_close_attempt, parent_runner_event_close_return,
    controller_control_close_attempt, controller_control_close_return,
    controller_event_close_attempt, controller_event_close_return,
    runner_event_close_attempt, runner_event_close_return,
    runner_control_close_attempt, runner_control_close_return,
    terminate_attempt, terminate_return, kill_attempt, kill_return,
    observed_exit, reap_attempt, reap_return,
    primary_failure, secondary_backend_retirement, secondary_runner_control,
    secondary_runner_event, secondary_controller_control, secondary_controller_event,
    secondary_graceful_termination, secondary_forced_escalation, secondary_exact_reap,
    clock_failed, callback_failed, cancel_write_failed, cancel_response_failed,
    count
};
static_assert(static_cast<unsigned>(SyntheticLifecycleMark::count) == 64U);

enum class SyntheticCaptureFailure : unsigned char {
    invalid_state, capacity, overflow, allocation, duplicate_mark, identity
};
enum class SyntheticCapturedReturn : unsigned char {
    absent, success, failed, exception, wrong_child, inconsistent_exit
};
enum class SyntheticCancelledPhase : unsigned char {
    startup, admission, service, execution, cleanup
};
struct SyntheticCapturedDeadline final {
    std::int64_t begin{}, duration{}, deadline{}, expiry{};
};
struct SyntheticCapturedExit final {
    bool signaled{};
    std::uint32_t value{};
};
struct SyntheticCapturedIdentity final {
    FinalizedEvidenceTransportIdentity pair;
    core::Sha256Digest request_body_sha256, capacity_offer_body_sha256,
        capacity_body_sha256, capacity_ack_body_sha256, prepared_offer_body_sha256,
        prepared_resource_sha256, prepared_ack_body_sha256, wire_registry_sha256,
        source_manifest_sha256;
};
static_assert(std::is_trivially_copyable_v<SyntheticCapturedIdentity>);
static_assert(std::is_nothrow_copy_assignable_v<SyntheticFailure>);

struct SyntheticCaptureLimits final {
    std::uint64_t request_bytes{}, offer_bytes{}, session_bytes{}, family_bytes{},
        text_bytes{}, capacity_elements{}, retained_bytes{}, peak_bytes{};
};
struct SyntheticCaptureUsage final {
    std::uint64_t request_bytes{}, offer_bytes{}, session_bytes{}, family_bytes{},
        text_bytes{}, capacity_elements{}, retained_bytes{}, peak_bytes{};
};

struct SyntheticLifecycleFacts final {
    SyntheticLifecycleFacts(PairwiseV2PairIdentity ordered_pair,
                            core::Sha256Digest authored_host_offer) noexcept
        : host_offer_body_sha256{authored_host_offer}, pair{ordered_pair} {}
    std::string request_body;
    std::optional<wire_v1::detail::AuthoredCapacityOffer> capacity_offer;
    core::Sha256Digest host_offer_body_sha256;
    PairwiseV2PairIdentity pair;
    SyntheticLifecycleConfig config;
    SyntheticCaptureUsage usage;
    std::array<std::uint8_t, 64U> ordinals{};
    std::uint8_t next_ordinal{1U};
    std::optional<SyntheticCaptureFailure> capture_failure;
    std::array<SyntheticCapturedDeadline, 5U> deadlines{};
    std::array<SyntheticCapturedReturn, 6U> closes{};
    SyntheticCapturedReturn terminate{}, kill{}, reap{};
    std::optional<SyntheticCapturedExit> observed_exit, reaped_exit;
    std::optional<SyntheticFailure> primary;
    std::array<SyntheticCleanupFailure, 8U> secondary{};
    std::size_t secondary_count{};
    std::optional<PairwiseV2CancellationReason> cancellation;
    std::optional<SyntheticCancelledPhase> cancelled_written_phase, cancelled_validated_phase;
    bool activation_intended{}, outcome_delivered{};
    std::optional<SyntheticCapturedIdentity> identity;

    [[nodiscard]] auto ordinal(SyntheticLifecycleMark mark) const noexcept -> std::uint8_t {
        return ordinals[static_cast<std::size_t>(mark)];
    }
    void mark(SyntheticLifecycleMark value) noexcept {
        const auto index = static_cast<std::size_t>(value);
        if (capture_failure) return;
        if (index >= ordinals.size() || next_ordinal > ordinals.size()) {
            capture_failure = SyntheticCaptureFailure::overflow;
        } else if (ordinals[index] != 0U) {
            capture_failure = SyntheticCaptureFailure::duplicate_mark;
        } else {
            ordinals[index] = next_ordinal++;
        }
    }
};

struct SyntheticCaptureOwner final {
    std::unique_ptr<SyntheticLifecycleFacts> facts;
    bool taken{};
};

inline constexpr std::size_t synthetic_capture_inline_bytes =
    sizeof(SyntheticCaptureOwner) + sizeof(std::span<const std::byte>);

// Named simultaneously live preparation cells, including both non-elided
// result headers. Dynamic ownership is accounted separately by usage().
struct SyntheticCaptureCopyStaging final {
    SyntheticCaptureUsage usage;
    std::uint64_t capacity_backing{}, elements{}, text{}, lane_bytes{};
    const wire_v1::detail::AuthoredCapacityOffer* original{};
    std::string_view request;
    std::unique_ptr<SyntheticLifecycleFacts> copied;
    std::expected<SyntheticCaptureUsage, SyntheticCaptureFailure> counted;
    std::expected<SyntheticCaptureUsage, SyntheticCaptureFailure> returned;
    std::optional<SyntheticCaptureFailure> failure;
};
struct SyntheticCapturePreparationInventory final {
    SyntheticCaptureCopyStaging caller, counter;
    const SyntheticCaptureLimits* limits;
    std::optional<core::Sha256Digest> host_digest;
    PairwiseV2PairIdentity constructor_pair;
    core::Sha256Digest constructor_host_digest;
};

// This is a one-use borrow, not a transferable runtime capability. The source
// lifecycle must outlive the view and its synchronous Results import.
class SyntheticLifecycleView final {
  public:
    SyntheticLifecycleView(const SyntheticLifecycleView&) = delete;
    SyntheticLifecycleView(SyntheticLifecycleView&& other) noexcept
        : facts_{std::exchange(other.facts_, nullptr)}, admitted_{other.admitted_},
          bytes_{other.bytes_} {}
    [[nodiscard]] auto facts() const noexcept -> const SyntheticLifecycleFacts* { return facts_; }
    [[nodiscard]] auto admitted() const noexcept -> const PairwiseV2AdmittedStartup* {
        return facts_ ? admitted_ : nullptr;
    }
    [[nodiscard]] auto canonical_bytes() const noexcept -> std::span<const std::byte> {
        return facts_ ? bytes_ : std::span<const std::byte>{};
    }
    // Call before validating an import; rejection cannot replay the handoff.
    [[nodiscard]] auto consume() noexcept -> SyntheticLifecycleView {
        return SyntheticLifecycleView{std::exchange(facts_, nullptr), admitted_, bytes_};
    }

  private:
    SyntheticLifecycleView(const SyntheticLifecycleFacts* facts,
                           const PairwiseV2AdmittedStartup* admitted,
                           std::span<const std::byte> bytes) noexcept
        : facts_{facts}, admitted_{admitted}, bytes_{bytes} {}
    const SyntheticLifecycleFacts* facts_{};
    const PairwiseV2AdmittedStartup* admitted_{};
    std::span<const std::byte> bytes_;
    friend class PairwiseV2SyntheticLifecycle;
};

// Counts every owning capacity lane before any copy. Defined beside the source
// recorder so the ordinary post-retirement consumer uses the same actual type.
[[nodiscard]] auto synthetic_capture_usage(
    const wire_v1::detail::AuthoredCapacityOffer&, std::string_view) noexcept
    -> std::expected<SyntheticCaptureUsage, SyntheticCaptureFailure>;

} // namespace epycbench::runtime::detail
