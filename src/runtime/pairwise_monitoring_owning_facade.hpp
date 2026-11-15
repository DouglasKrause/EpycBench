#pragma once

#include "epycbench/runtime/pairwise_evidence_failure.hpp"
#include "epycbench/monitoring/acquisition_driver.hpp"
#include "runtime/pairwise_monitoring_capacity.hpp"
#include "runtime/pairwise_monitoring_preparation_facts.hpp"
#include "runtime/pairwise_platform_leaf_facade.hpp"
#include "epycbench/runtime/pairwise_finalized_error_profile.hpp"
#include "monitoring/pairwise_prepared_capture_storage.hpp"

#include <optional>
#include <array>
#include <compare>
#include <memory>
#include <thread>
#include <span>
#include <stop_token>
#include <variant>
#include <vector>

namespace epycbench::runtime::detail {

struct PairwiseMonitoringOperationFailureAccess;
class PairwiseWorkerReadinessCapability;

enum class PairwiseMonitoringAuthorshipDisposition : unsigned char {
    direct_monitoring_branch,
    delegated_platform_branch,
    delegated_service_placement_branch,
    delegated_clock_branch,
    precluded_by_authenticated_precondition,
};

enum class PairwiseMonitoringProducerFamily : unsigned char {
    required_source,
    driver_transition,
    scheduled_capacity,
    provider_prepare_containment,
    service_containment,
    stop_containment,
    service_placement,
    page_classifier,
    context_classifier,
    temperature_classifier,
    frequency_classifier,
    platform_task_stat,
    platform_perf_context_switch,
    platform_hwmon_temperature,
    platform_cpu_frequency,
    platform_service_placement,
    platform_raw_clock,
    admitted_policy_precondition,
    prepared_operations_precondition,
    typed_platform_precondition,
    noexcept_driver_precondition,
};

// Stable compositional-proof identifiers. The source audit binds each value
// to the exact lower or predecessor test source that supplies its evidence.
enum class PairwiseMonitoringEvidenceGate : unsigned char {
    owning_facade_direct_branch,
    platform_leaf_task_stat_branches,
    platform_leaf_perf_branches,
    platform_leaf_hwmon_branches,
    platform_leaf_cpu_frequency_branches,
    platform_leaf_service_placement_branches,
    platform_leaf_raw_clock_branches,
    production_admission_policy_precondition,
    source_transaction_operations_precondition,
    platform_capability_precondition,
    noexcept_lifecycle_precondition,
};

struct PairwiseMonitoringAuthorshipEntry final {
    PairwiseFinalizedErrorTuple tuple;
    PairwiseFailureContext context;
    std::optional<int> native_error;
    PairwiseMonitoringAuthorshipDisposition disposition;
    PairwiseMonitoringProducerFamily producer;
    PairwiseMonitoringEvidenceGate evidence_gate;
    std::uint16_t branch;
};

struct PairwiseMonitoringAuthoredTuple final {
    PairwiseFailureCode code;
    PairwiseFailureOperation operation;
    PairwiseFailureReason reason;
    PairwiseFailureContext context;
    std::optional<int> native_error;
};

[[nodiscard]] auto pairwise_monitoring_authorship_manifest() noexcept
    -> std::span<const PairwiseMonitoringAuthorshipEntry>;

struct PairwiseMonitoringAuthorshipTestAccess final {
    [[nodiscard]] static auto invoke_direct(
        const PairwiseMonitoringAuthorshipEntry& entry) noexcept
        -> std::optional<PairwiseMonitoringAuthoredTuple>;
};

class PairwiseMonitoringOwningFacade;
class PairwiseMonitoringCaptureRegistration;
class PairwiseMonitoringCaptureFailureCursor;

class PairwiseMonitoringTerminalCapture final {
  public:
    PairwiseMonitoringTerminalCapture(const PairwiseMonitoringTerminalCapture&) = delete;
    PairwiseMonitoringTerminalCapture(PairwiseMonitoringTerminalCapture&&) noexcept;
    auto operator=(const PairwiseMonitoringTerminalCapture&)
        -> PairwiseMonitoringTerminalCapture& = delete;
    auto operator=(PairwiseMonitoringTerminalCapture&&)
        -> PairwiseMonitoringTerminalCapture& = delete;
    ~PairwiseMonitoringTerminalCapture() noexcept;
    [[nodiscard]] auto result() const noexcept
        -> const monitoring::MonitoringCaptureResult& {
        return monitoring::detail::PairwisePreparedCaptureStorageAccess::result(
            slot_);
    }
    // Consumes the terminal capability and transfers its already-preallocated
    // result.  No nested evidence container is copied or reallocated.
    [[nodiscard]] auto take_result() && noexcept
        -> std::optional<monitoring::MonitoringCaptureResult> {
        if (result_taken_)
            return std::nullopt;
        result_taken_ = true;
        return monitoring::detail::PairwisePreparedCaptureStorageAccess::take_result(
            slot_);
    }
  private:
    explicit PairwiseMonitoringTerminalCapture(
        monitoring::detail::PairwisePreparedCaptureSlot slot) noexcept
        : slot_{std::move(slot)} {}
    monitoring::detail::PairwisePreparedCaptureSlot slot_;
    bool result_taken_{};
    friend class PairwiseMonitoringOwningFacade;
};

// Closed lower capability for the inactive Monitoring graph.  It is not a
// generic subsystem sum: Platform values are admitted only from the exact
// production Monitoring delegate families.
class PairwiseMonitoringOperationFailure final {
  public:
    PairwiseMonitoringOperationFailure(const PairwiseMonitoringOperationFailure&) noexcept = default;
    PairwiseMonitoringOperationFailure(PairwiseMonitoringOperationFailure&&) noexcept = default;
    auto operator=(const PairwiseMonitoringOperationFailure&)
        -> PairwiseMonitoringOperationFailure& = delete;
    auto operator=(PairwiseMonitoringOperationFailure&&)
        -> PairwiseMonitoringOperationFailure& = delete;

    [[nodiscard]] static auto from_monitoring(PairwiseMonitoringFailure failure) noexcept
        -> PairwiseMonitoringOperationFailure;
    [[nodiscard]] static auto from_platform(PairwisePlatformFailure failure) noexcept
        -> std::optional<PairwiseMonitoringOperationFailure>;
    [[nodiscard]] static auto from_service_platform(
        PairwisePlatformFailure failure) noexcept
        -> std::optional<PairwiseMonitoringOperationFailure>;

    [[nodiscard]] auto subsystem() const noexcept -> PairwiseFailureSubsystem;
    [[nodiscard]] auto code() const noexcept -> PairwiseFailureCode;
    [[nodiscard]] auto operation() const noexcept -> PairwiseFailureOperation;
    [[nodiscard]] auto reason() const noexcept -> PairwiseFailureReason;
    [[nodiscard]] auto context() const noexcept -> const PairwiseFailureContext&;
    [[nodiscard]] auto posix_errno() const noexcept -> std::optional<int>;

  private:
    using Value = std::variant<PairwiseMonitoringFailure, PairwisePlatformFailure>;
    explicit PairwiseMonitoringOperationFailure(Value value) noexcept
        : value_{std::move(value)} {}
    Value value_;
    friend class PairwiseMonitoringOwningFacade;
    friend struct PairwiseMonitoringOperationFailureAccess;
    friend class PairwiseMonitoringFailureCursor;
    friend class PairwiseMonitoringCaptureFailureCursor;
};

// Raw-clock failures are deliberately separate from the exact 48-relation
// Monitoring source delegate. The scheduler passes through only these five
// registered Platform relations unchanged.
class PairwiseMonitoringSchedulerFailure final {
  public:
    PairwiseMonitoringSchedulerFailure(
        const PairwiseMonitoringSchedulerFailure&) noexcept = default;
    PairwiseMonitoringSchedulerFailure(
        PairwiseMonitoringSchedulerFailure&&) noexcept = default;
    [[nodiscard]] static auto from_platform(
        PairwisePlatformFailure) noexcept
        -> std::optional<PairwiseMonitoringSchedulerFailure>;
    [[nodiscard]] auto value() const noexcept
        -> const PairwisePlatformFailure& { return value_; }
  private:
    explicit PairwiseMonitoringSchedulerFailure(
        PairwisePlatformFailure value) noexcept : value_{std::move(value)} {}
    PairwisePlatformFailure value_;
    friend class PairwiseMonitoringCaptureFailureCursor;
};

using PairwiseMonitoringAcquisitionFailure =
    std::variant<PairwiseMonitoringOperationFailure,
                 PairwiseMonitoringSchedulerFailure>;

struct PairwiseMonitoringPageReadDisposition final {
    bool attempted{};
    bool close_failed{};
    int native_error{};
};

class PairwiseMonitoringCaptureFailure final {
  public:
    PairwiseMonitoringCaptureFailure(const PairwiseMonitoringCaptureFailure&) = delete;
    PairwiseMonitoringCaptureFailure(PairwiseMonitoringCaptureFailure&&) noexcept = default;
    [[nodiscard]] auto primary() const noexcept
        -> const PairwiseMonitoringAcquisitionFailure& { return primary_; }
    [[nodiscard]] auto page_close_disposition() const noexcept
        -> const std::optional<PairwiseMonitoringPageReadDisposition>& {
        return page_close_disposition_;
    }
    [[nodiscard]] auto cleanup_count() const noexcept -> std::size_t {
        return cleanup_count_;
    }
    [[nodiscard]] auto cleanup(std::size_t index) const noexcept
        -> const PairwiseMonitoringOperationFailure* {
        const auto position = cleanup_offset_ + index;
        return index < cleanup_count_ && position < cleanup_.size() &&
                cleanup_[position]
            ? &*cleanup_[position]
            : nullptr;
    }
    PairwiseMonitoringCaptureFailure(
        PairwiseMonitoringAcquisitionFailure primary,
        std::optional<PairwiseMonitoringPageReadDisposition> disposition =
            std::nullopt,
        std::vector<std::optional<PairwiseMonitoringOperationFailure>> cleanup = {},
        std::size_t cleanup_offset = 0U,
        std::size_t cleanup_count = 0U) noexcept
        : primary_{std::move(primary)},
          page_close_disposition_{disposition}, cleanup_{std::move(cleanup)},
          cleanup_offset_{cleanup_offset}, cleanup_count_{cleanup_count} {}
    [[nodiscard]] static auto with_cleanup(
        PairwiseMonitoringCaptureFailure primary,
        std::vector<std::optional<PairwiseMonitoringOperationFailure>> cleanup,
        std::size_t cleanup_count) noexcept
        -> PairwiseMonitoringCaptureFailure {
        return PairwiseMonitoringCaptureFailure{
            std::move(primary.primary_), primary.page_close_disposition_,
            std::move(cleanup), 0U, cleanup_count};
    }
  private:
    PairwiseMonitoringAcquisitionFailure primary_;
    std::optional<PairwiseMonitoringPageReadDisposition>
        page_close_disposition_;
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> cleanup_;
    std::size_t cleanup_offset_{};
    std::size_t cleanup_count_{};
    friend class PairwiseMonitoringOwningFacade;
    friend class PairwiseMonitoringCaptureFailureCursor;
};

// Consuming, allocation-free projection of one capture terminal. The primary
// is yielded first, followed by every lower cleanup in its original order.
class PairwiseMonitoringCaptureFailureCursor final {
  public:
    PairwiseMonitoringCaptureFailureCursor(
        const PairwiseMonitoringCaptureFailureCursor&) = delete;
    PairwiseMonitoringCaptureFailureCursor(
        PairwiseMonitoringCaptureFailureCursor&&) noexcept;
    auto operator=(const PairwiseMonitoringCaptureFailureCursor&)
        -> PairwiseMonitoringCaptureFailureCursor& = delete;
    auto operator=(PairwiseMonitoringCaptureFailureCursor&&)
        -> PairwiseMonitoringCaptureFailureCursor& = delete;

    [[nodiscard]] static auto consume(
        PairwiseMonitoringCaptureFailure&&) noexcept
        -> PairwiseMonitoringCaptureFailureCursor;
    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto next() noexcept -> PairwiseEvidenceFailure;

  private:
    explicit PairwiseMonitoringCaptureFailureCursor(
        PairwiseMonitoringCaptureFailure&& source) noexcept
        : source_{std::move(source)} {}
    [[nodiscard]] static auto project(
        PairwiseMonitoringOperationFailure&) noexcept
        -> PairwiseEvidenceFailure;
    void require_active() const noexcept;
    PairwiseMonitoringCaptureFailure source_;
    std::size_t cleanup_index_{};
    bool primary_pending_{true};
    bool active_{true};
};

class PairwiseMonitoringActiveCapture final {
  public:
    PairwiseMonitoringActiveCapture(const PairwiseMonitoringActiveCapture&) = delete;
    PairwiseMonitoringActiveCapture(PairwiseMonitoringActiveCapture&& other) noexcept
        : state_{other.state_}, generation_{other.generation_},
          registrations_{std::move(other.registrations_)},
          registration_index_{other.registration_index_} {
        other.state_ = nullptr;
    }
    auto operator=(const PairwiseMonitoringActiveCapture&)
        -> PairwiseMonitoringActiveCapture& = delete;
    auto operator=(PairwiseMonitoringActiveCapture&&)
        -> PairwiseMonitoringActiveCapture& = delete;
    ~PairwiseMonitoringActiveCapture() noexcept;
    [[nodiscard]] auto generation() const noexcept -> std::uint64_t {
        return generation_;
    }
  private:
    PairwiseMonitoringActiveCapture(
        void* state, std::uint64_t generation,
        std::shared_ptr<PairwiseMonitoringCaptureRegistration[]> registrations,
        std::size_t registration_index) noexcept
        : state_{state}, generation_{generation},
          registrations_{std::move(registrations)},
          registration_index_{registration_index} {}
    [[nodiscard]] auto registration() const noexcept
        -> PairwiseMonitoringCaptureRegistration*;
    void* state_{};
    std::uint64_t generation_{};
    std::shared_ptr<PairwiseMonitoringCaptureRegistration[]> registrations_;
    std::size_t registration_index_{};
    friend class PairwiseMonitoringOwningFacade;
};

struct PairwiseMonitoringForceDisposition final {
    std::uint8_t attempt{};
    std::uint8_t outcome{};
    bool ownership_remains{};
};

enum class PairwiseMonitoringServiceJoinFault : std::uint8_t {
    success,
    native_failure,
    contained_standard,
    contained_unknown,
};

class PairwiseMonitoringPreparationFailure final {
  public:
    PairwiseMonitoringPreparationFailure(const PairwiseMonitoringPreparationFailure&) = delete;
    PairwiseMonitoringPreparationFailure(PairwiseMonitoringPreparationFailure&&) noexcept;
    [[nodiscard]] static auto runtime(PairwiseRuntimeFailure) noexcept
        -> PairwiseMonitoringPreparationFailure;
    [[nodiscard]] static auto with_source_retirement(
        PairwiseMonitoringPreparationFailure,
        std::vector<platform_linux::detail::BoundedSysfsDisposition>,
        std::size_t, std::size_t) noexcept
        -> PairwiseMonitoringPreparationFailure;
    [[nodiscard]] static auto with_service_terminalization(
        PairwiseMonitoringPreparationFailure,
        PairwiseMonitoringServiceJoinFault,
        std::array<PairwiseMonitoringForceDisposition, 3>,
        std::size_t) noexcept -> PairwiseMonitoringPreparationFailure;
    [[nodiscard]] static auto operation(
        PairwiseMonitoringOperationFailure,
        std::vector<PairwiseMonitoringOperationFailure> cleanup = {},
        std::vector<platform_linux::detail::BoundedSysfsDisposition>
            source_retirement = {},
        std::size_t source_retirement_count = 0U,
        std::size_t frequency_source_retirement_count = 0U) noexcept
        -> PairwiseMonitoringPreparationFailure;
    [[nodiscard]] auto subsystem() const noexcept -> PairwiseFailureSubsystem;
    [[nodiscard]] auto code() const noexcept -> PairwiseFailureCode;
    [[nodiscard]] auto operation_name() const noexcept -> PairwiseFailureOperation;
    [[nodiscard]] auto reason() const noexcept -> PairwiseFailureReason;
    [[nodiscard]] auto posix_errno() const noexcept -> std::optional<int>;
    [[nodiscard]] auto cleanup() const noexcept
        -> std::span<const PairwiseMonitoringOperationFailure> {
        require_active();
        return cleanup_;
    }
    [[nodiscard]] auto source_retirement() const noexcept
        -> std::span<const platform_linux::detail::BoundedSysfsDisposition> {
        require_active();
        return {source_retirement_.data(), source_retirement_count_};
    }
    [[nodiscard]] auto ordinary_service_join() const noexcept
        -> std::optional<PairwiseMonitoringServiceJoinFault> {
        require_active();
        return ordinary_service_join_;
    }
    [[nodiscard]] auto force_disposition_count() const noexcept -> std::size_t {
        require_active();
        return force_disposition_count_;
    }
    [[nodiscard]] auto force_disposition(std::size_t index) const noexcept
        -> const PairwiseMonitoringForceDisposition* {
        require_active();
        return index < force_disposition_count_ ? &force_dispositions_[index]
                                                : nullptr;
    }
  private:
    using Value = std::variant<PairwiseRuntimeFailure,
                               PairwiseMonitoringOperationFailure>;
    explicit PairwiseMonitoringPreparationFailure(
        Value value,
        std::vector<PairwiseMonitoringOperationFailure> cleanup = {},
        std::vector<platform_linux::detail::BoundedSysfsDisposition>
            source_retirement = {},
        std::size_t source_retirement_count = 0U,
        std::size_t frequency_source_retirement_count = 0U) noexcept
        : value_{std::move(value)}, cleanup_{std::move(cleanup)},
          source_retirement_{std::move(source_retirement)},
          source_retirement_count_{source_retirement_count},
          frequency_source_retirement_count_{
              frequency_source_retirement_count} {}
    void require_active() const noexcept;
    Value value_;
    std::vector<PairwiseMonitoringOperationFailure> cleanup_;
    std::vector<platform_linux::detail::BoundedSysfsDisposition>
        source_retirement_;
    std::size_t source_retirement_count_{};
    std::size_t frequency_source_retirement_count_{};
    std::optional<PairwiseMonitoringServiceJoinFault> ordinary_service_join_;
    std::array<PairwiseMonitoringForceDisposition, 3> force_dispositions_{};
    std::size_t force_disposition_count_{};
    bool active_{true};
    friend class PairwiseMonitoringFailureCursor;
};

class PairwiseMonitoringStopOutcome final {
  public:
    PairwiseMonitoringStopOutcome(const PairwiseMonitoringStopOutcome&) = delete;
    PairwiseMonitoringStopOutcome(PairwiseMonitoringStopOutcome&&) noexcept;
    [[nodiscard]] auto failure_count() const noexcept -> std::size_t {
        require_active();
        return failure_count_;
    }
    [[nodiscard]] auto invariant_failed() const noexcept -> bool {
        require_active();
        return invariant_failed_;
    }
    [[nodiscard]] auto failure(std::size_t index) const noexcept
        -> const PairwiseMonitoringOperationFailure* {
        require_active();
        return index < failure_count_ && failures_[index]
            ? &*failures_[index]
            : nullptr;
    }
    [[nodiscard]] auto source_retirement() const noexcept
        -> std::span<const platform_linux::detail::BoundedSysfsDisposition> {
        require_active();
        return {source_retirement_.data(), source_retirement_count_};
    }
    [[nodiscard]] auto force_disposition_count() const noexcept -> std::size_t {
        require_active();
        return force_disposition_count_;
    }
    [[nodiscard]] auto force_disposition(std::size_t index) const noexcept
        -> const PairwiseMonitoringForceDisposition* {
        require_active();
        return index < force_disposition_count_ ? &force_dispositions_[index]
                                                : nullptr;
    }
  private:
    PairwiseMonitoringStopOutcome(
        std::vector<std::optional<PairwiseMonitoringOperationFailure>> failures,
        std::size_t failure_count,
        std::vector<platform_linux::detail::BoundedSysfsDisposition>
            source_retirement,
        std::size_t source_retirement_count,
        std::size_t frequency_source_retirement_count,
        std::array<PairwiseMonitoringForceDisposition, 3> force_dispositions,
        std::size_t force_disposition_count,
        bool invariant_failed) noexcept
        : failures_{std::move(failures)}, failure_count_{failure_count},
          source_retirement_{std::move(source_retirement)},
          source_retirement_count_{source_retirement_count},
          frequency_source_retirement_count_{
              frequency_source_retirement_count},
          force_dispositions_{force_dispositions},
          force_disposition_count_{force_disposition_count},
          invariant_failed_{invariant_failed} {}
    void require_active() const noexcept;
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> failures_;
    std::size_t failure_count_{};
    std::vector<platform_linux::detail::BoundedSysfsDisposition>
        source_retirement_;
    std::size_t source_retirement_count_{};
    std::size_t frequency_source_retirement_count_{};
    std::array<PairwiseMonitoringForceDisposition, 3> force_dispositions_{};
    std::size_t force_disposition_count_{};
    bool invariant_failed_{};
    bool active_{true};
    friend class PairwiseMonitoringOwningFacade;
    friend class PairwiseMonitoringFailureCursor;
    friend struct PairwiseMonitoringPreparationTestAccess;
};

// Source-private one-shot projection over a real Monitoring terminal.  It
// owns the consumed lower terminal and yields its exact closed failures in
// cleanup order without allocating or exposing a lower mutable alternative.
class PairwiseMonitoringFailureCursor final {
  public:
    PairwiseMonitoringFailureCursor(const PairwiseMonitoringFailureCursor&) = delete;
    PairwiseMonitoringFailureCursor(PairwiseMonitoringFailureCursor&&) noexcept;
    auto operator=(const PairwiseMonitoringFailureCursor&)
        -> PairwiseMonitoringFailureCursor& = delete;
    auto operator=(PairwiseMonitoringFailureCursor&&)
        -> PairwiseMonitoringFailureCursor& = delete;
    ~PairwiseMonitoringFailureCursor() noexcept = default;

    [[nodiscard]] static auto consume(
        PairwiseMonitoringPreparationFailure&&) noexcept
        -> PairwiseMonitoringFailureCursor;
    [[nodiscard]] static auto consume(PairwiseMonitoringStopOutcome&&) noexcept
        -> PairwiseMonitoringFailureCursor;
    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto next() noexcept -> PairwiseEvidenceFailure;

  private:
    enum class Kind : std::uint8_t { preparation, stop };
    explicit PairwiseMonitoringFailureCursor(
        PairwiseMonitoringPreparationFailure&&) noexcept;
    explicit PairwiseMonitoringFailureCursor(
        PairwiseMonitoringStopOutcome&&) noexcept;
    void require_active() const noexcept;
    [[nodiscard]] auto preparation_empty() const noexcept -> bool;
    [[nodiscard]] auto stop_empty() const noexcept -> bool;
    [[nodiscard]] auto next_preparation() noexcept -> PairwiseEvidenceFailure;
    [[nodiscard]] auto next_stop() noexcept -> PairwiseEvidenceFailure;
    [[nodiscard]] static auto project_operation(
        PairwiseMonitoringOperationFailure&&) noexcept
        -> PairwiseEvidenceFailure;

    Kind kind_{};
    std::variant<PairwiseMonitoringPreparationFailure,
                 PairwiseMonitoringStopOutcome> source_;
    std::size_t operation_index_{};
    std::size_t source_index_{};
    bool primary_pending_{};
    bool ordinary_pending_{};
    bool invariant_pending_{};
    bool active_{true};
};

enum class PairwiseMonitoringServiceRunFault : std::uint8_t {
    none,
    contained_standard,
    contained_unknown,
};

enum class PairwiseMonitoringPreparationFault : std::uint8_t {
    none,
    reject_platform_mapping,
    exhaust_typed_cleanup,
    exhaust_source_disposition,
    mismatch_temperature_owner,
    mismatch_frequency_owner,
    contained_after_owner,
    service_construction,
    missing_service_placement,
    missing_service_placement_join_native,
    missing_service_placement_join_contained_standard,
    missing_service_placement_join_contained_unknown,
    missing_service_placement_join_forced_second,
    missing_service_placement_join_terminal,
};

struct BoundedMonitoringStorageDependencies final {
    platform_linux::detail::BoundedHwmonCapacity temperature_capacity{};
    platform_linux::detail::BoundedSysfsOperations temperature_operations{};
    platform_linux::detail::BoundedCpuFrequencyCapacity frequency_capacity{};
    std::span<const core::LinuxCpuId> present_cpus{};
    std::span<const core::LinuxCpuId> worker_cpus{};
    platform_linux::detail::BoundedSysfsOperations frequency_operations{};
};

class PairwiseBoundMonitoringPreparation;

class PairwisePreparedMonitoringStorage final {
  public:
    struct State;
    PairwisePreparedMonitoringStorage(
        const PairwisePreparedMonitoringStorage&) = delete;
    PairwisePreparedMonitoringStorage(
        PairwisePreparedMonitoringStorage&&) noexcept;
    auto operator=(const PairwisePreparedMonitoringStorage&)
        -> PairwisePreparedMonitoringStorage& = delete;
    auto operator=(PairwisePreparedMonitoringStorage&&) noexcept
        -> PairwisePreparedMonitoringStorage&;
    ~PairwisePreparedMonitoringStorage() noexcept;

    [[nodiscard]] auto bind_worker_readiness(
        PairwiseWorkerReadinessCapability) && noexcept
        -> std::expected<PairwiseBoundMonitoringPreparation,
                         PairwiseMonitoringPreparationFailure>;

  private:
    explicit PairwisePreparedMonitoringStorage(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend class PairwiseMonitoringOwningFacade;
    friend struct PairwiseMonitoringPreparationTestAccess;
    friend class PairwiseBoundMonitoringPreparation;
};

class PairwiseBoundMonitoringPreparation final {
  public:
    PairwiseBoundMonitoringPreparation(
        const PairwiseBoundMonitoringPreparation&) = delete;
    PairwiseBoundMonitoringPreparation(
        PairwiseBoundMonitoringPreparation&&) noexcept;
    auto operator=(const PairwiseBoundMonitoringPreparation&)
        -> PairwiseBoundMonitoringPreparation& = delete;
    auto operator=(PairwiseBoundMonitoringPreparation&&) noexcept
        -> PairwiseBoundMonitoringPreparation&;
    ~PairwiseBoundMonitoringPreparation() noexcept;

    [[nodiscard]] auto acquire_sources_and_owner(
        const PairwisePlatformLeafFacade&) && noexcept
        -> std::expected<PairwiseMonitoringOwningFacade,
                         PairwiseMonitoringPreparationFailure>;

  private:
    explicit PairwiseBoundMonitoringPreparation(
        std::unique_ptr<PairwisePreparedMonitoringStorage::State>) noexcept;
    std::unique_ptr<PairwisePreparedMonitoringStorage::State> state_;
    friend class PairwisePreparedMonitoringStorage;
    friend struct PairwiseMonitoringPreparationTestAccess;
};

// Inactive prepared owner. It allocates every capacity-derived destination,
// cleanup slot and control object before a lower persistent owner or service
// thread may be installed. No selected production route consumes this type.
class PairwiseMonitoringOwningFacade final {
  public:
    struct State;
    PairwiseMonitoringOwningFacade(const PairwiseMonitoringOwningFacade&) = delete;
    PairwiseMonitoringOwningFacade(PairwiseMonitoringOwningFacade&&) noexcept;
    auto operator=(const PairwiseMonitoringOwningFacade&)
        -> PairwiseMonitoringOwningFacade& = delete;
    auto operator=(PairwiseMonitoringOwningFacade&&)
        -> PairwiseMonitoringOwningFacade& = delete;
    ~PairwiseMonitoringOwningFacade() noexcept;
    [[nodiscard]] auto retired_preparation_facts() const noexcept
        -> std::optional<MonitoringPreparationFactView>;

    [[nodiscard]] static auto prepare_capacity_storage(
        PairwiseAdmittedMonitoringCapacity,
        const PairwisePlatformLeafFacade&,
        BoundedMonitoringStorageDependencies) noexcept
        -> std::expected<PairwisePreparedMonitoringStorage,
                         PairwiseMonitoringPreparationFailure>;

    [[nodiscard]] static auto prepare_storage(
        PairwiseAdmittedMonitoringCapacity,
        std::span<const platform_linux::LinuxThreadId>,
        std::optional<platform_linux::detail::BoundedHwmonValidatedOutcome>,
        std::optional<
            platform_linux::detail::BoundedCpuFrequencyValidatedGraphOutcome>) noexcept
        -> std::expected<PairwiseMonitoringOwningFacade,
                         PairwiseMonitoringPreparationFailure>;
    [[nodiscard]] static auto prepare_sources(
        PairwiseAdmittedMonitoringCapacity,
        std::span<const platform_linux::LinuxThreadId>,
        const PairwisePlatformLeafFacade&,
        platform_linux::detail::BoundedHwmonCapacity,
        platform_linux::detail::BoundedSysfsOperations,
        platform_linux::detail::BoundedCpuFrequencyCapacity,
        std::span<const core::LinuxCpuId> present_cpus,
        std::span<const core::LinuxCpuId> worker_cpus,
        platform_linux::detail::BoundedSysfsOperations) noexcept
        -> std::expected<PairwiseMonitoringOwningFacade,
                         PairwiseMonitoringPreparationFailure>;

    [[nodiscard]] auto capacity() const noexcept
        -> const PairwiseAdmittedMonitoringCapacity&;
    [[nodiscard]] auto workers() const noexcept
        -> std::span<const monitoring::MonitoringWorkerScope>;
    [[nodiscard]] auto capture_slot_count() const noexcept -> std::size_t;
    [[nodiscard]] auto available_capture_slot_count() const noexcept -> std::size_t;
    [[nodiscard]] auto take_capture_slot(std::size_t) noexcept
        -> std::optional<PairwiseMonitoringTerminalCapture>;
    [[nodiscard]] auto stop_and_retire(
        const PairwisePlatformLeafFacade&) noexcept
        -> PairwiseMonitoringStopOutcome;
    [[nodiscard]] auto begin_capture(
        const PairwisePlatformLeafFacade&) noexcept
        -> std::expected<PairwiseMonitoringActiveCapture,
                         PairwiseMonitoringCaptureFailure>;
    [[nodiscard]] auto complete_capture(
        PairwiseMonitoringActiveCapture,
        platform_linux::RawMonotonicTimestamp observation_start,
        platform_linux::RawMonotonicTimestamp observation_end,
        const PairwisePlatformLeafFacade&) noexcept
        -> std::expected<PairwiseMonitoringTerminalCapture,
                         PairwiseMonitoringCaptureFailure>;
    [[nodiscard]] auto abort_capture(PairwiseMonitoringActiveCapture) noexcept
        -> std::expected<void, PairwiseMonitoringCaptureFailure>;
    [[nodiscard]] auto context_offset_slot_count() const noexcept -> std::size_t;
    [[nodiscard]] auto context_slot_count() const noexcept -> std::size_t;
    [[nodiscard]] auto context_event_slot_count() const noexcept -> std::size_t;
    [[nodiscard]] auto frequency_nested_cpu_slot_count() const noexcept
        -> std::size_t;
    [[nodiscard]] auto frequency_supported() const noexcept -> bool;
    [[nodiscard]] auto persistent_frequency_cpu_capacity() const noexcept
        -> std::size_t;
    [[nodiscard]] auto rollback_returned_capacity() const noexcept -> std::size_t;
    [[nodiscard]] auto completion_returned_capacity() const noexcept -> std::size_t;
    [[nodiscard]] auto stop_returned_capacity() const noexcept -> std::size_t;
    [[nodiscard]] auto page_read_disposition_capacity() const noexcept
        -> std::size_t;
    [[nodiscard]] auto force_disposition_capacity() const noexcept -> std::size_t;
    [[nodiscard]] auto force_disposition_count() const noexcept -> std::size_t;
    [[nodiscard]] auto has_service_owner() const noexcept -> bool;

  private:
    [[nodiscard]] static auto prepare_sources_with_fault(
        PairwiseAdmittedMonitoringCapacity,
        std::span<const platform_linux::LinuxThreadId>,
        const PairwisePlatformLeafFacade&,
        platform_linux::detail::BoundedHwmonCapacity,
        platform_linux::detail::BoundedSysfsOperations,
        platform_linux::detail::BoundedCpuFrequencyCapacity,
        std::span<const core::LinuxCpuId>,
        std::span<const core::LinuxCpuId>,
        platform_linux::detail::BoundedSysfsOperations,
        PairwiseMonitoringPreparationFault) noexcept
        -> std::expected<PairwiseMonitoringOwningFacade,
                         PairwiseMonitoringPreparationFailure>;
    [[nodiscard]] static auto prepare_storage_impl(
        PairwiseAdmittedMonitoringCapacity,
        std::span<const platform_linux::LinuxThreadId>,
        bool temperature_supported,
        platform_linux::detail::BoundedCpuFrequencyValidatedDisposition,
        std::span<const std::size_t>, bool bind_workers = true,
        bool maximum_layout = false) noexcept
        -> std::expected<PairwiseMonitoringOwningFacade,
                         PairwiseMonitoringPreparationFailure>;
    [[nodiscard]] auto progress_quiescent(bool retired = false) const noexcept -> bool;
    [[nodiscard]] auto preparation_graph_facts() const noexcept -> std::optional<MonitoringPreparationFactView>;
    friend class PairwiseTypedOwnerTransaction;
    explicit PairwiseMonitoringOwningFacade(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend struct PairwiseMonitoringPreparationTestAccess;
    friend class PairwiseBoundMonitoringPreparation;
};

struct PairwiseMonitoringPreparationTestAccess final {
    struct StorageIdentity final {
        const void* state{};
        const void* workers{};
        std::size_t worker_capacity{};
        const void* capture_slots{};
        const void* classifier_scratch{};
        const void* registrations{};
        std::size_t registration_count{};
        const void* frequency_cpus{};
        std::size_t frequency_cpu_capacity{};
        std::size_t frequency_metadata_count{};
        const void* rollback_cleanup{};
        std::size_t rollback_cleanup_capacity{};
        bool frequency_partition_committed{};
        monitoring::detail::PairwisePreparedFrequencyLayoutObservation
            frequency_layout{};
        auto operator<=>(const StorageIdentity&) const = default;
    };
    [[nodiscard]] static auto storage_identity(
        const PairwisePreparedMonitoringStorage&) noexcept -> StorageIdentity;
    [[nodiscard]] static auto storage_identity(
        const PairwiseBoundMonitoringPreparation&) noexcept -> StorageIdentity;
    [[nodiscard]] static auto storage_identity(
        const PairwiseMonitoringOwningFacade&) noexcept -> StorageIdentity;
    [[nodiscard]] static auto rollback_cleanup_capacity(
        bool temperature_requested, bool frequency_requested,
        std::size_t domains, std::size_t rollback_values) noexcept
        -> std::optional<std::size_t>;
    [[nodiscard]] static auto prepare_sources(
        PairwiseAdmittedMonitoringCapacity capacity,
        std::span<const platform_linux::LinuxThreadId> threads,
        const PairwisePlatformLeafFacade& platform,
        platform_linux::detail::BoundedHwmonCapacity temperature_capacity,
        platform_linux::detail::BoundedSysfsOperations temperature_operations,
        platform_linux::detail::BoundedCpuFrequencyCapacity frequency_capacity,
        std::span<const core::LinuxCpuId> present_cpus,
        std::span<const core::LinuxCpuId> worker_cpus,
        platform_linux::detail::BoundedSysfsOperations frequency_operations,
        PairwiseMonitoringPreparationFault fault) noexcept {
        return PairwiseMonitoringOwningFacade::prepare_sources_with_fault(
            std::move(capacity), threads, platform, temperature_capacity,
            temperature_operations, frequency_capacity, present_cpus,
            worker_cpus, frequency_operations, fault);
    }
    static auto set_service_join_script(
        PairwiseMonitoringOwningFacade& facade,
        std::span<const PairwiseMonitoringServiceJoinFault> outcomes) noexcept
        -> bool;
    static auto set_service_run_fault(
        PairwiseMonitoringOwningFacade& facade,
        PairwiseMonitoringServiceRunFault outcome) noexcept -> bool;
    [[nodiscard]] static auto active_activity(
        const PairwiseMonitoringOwningFacade& facade) noexcept
        -> std::optional<monitoring::MonitoringDriverActivityEvidence>;
    [[nodiscard]] static auto active_first_temperature_start(
        const PairwiseMonitoringOwningFacade& facade) noexcept
        -> std::optional<platform_linux::RawMonotonicTimestamp>;
    [[nodiscard]] static auto invariant_stop_outcome_for_projection() noexcept
        -> PairwiseMonitoringStopOutcome;
};

} // namespace epycbench::runtime::detail
