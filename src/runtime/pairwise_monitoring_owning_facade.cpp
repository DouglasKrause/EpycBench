#include "runtime/pairwise_monitoring_owning_facade.hpp"
#include "runtime/pairwise_worker_owning_facade.hpp"

#include "monitoring/page_fault_classifier_kernel.hpp"
#include "monitoring/context_switch_classifier_kernel.hpp"
#include "monitoring/interruptible_raw_scheduler_kernel.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

[[maybe_unused]] constexpr auto pairwise_schedule_capacity_source_anchor =
    "monitoring.driver.scheduled_capacity_exceeded";

[[nodiscard]] constexpr auto monitoring_platform_code(
    PairwiseFailureCode code) noexcept -> bool {
    switch (code) {
    case PairwiseFailureCode::platform_task_stat_read_failed:
    case PairwiseFailureCode::platform_task_stat_malformed:
    case PairwiseFailureCode::platform_task_stat_counter_out_of_range:
    case PairwiseFailureCode::platform_task_stat_identity_mismatch:
    case PairwiseFailureCode::platform_perf_context_switch_invalid_configuration:
    case PairwiseFailureCode::platform_perf_context_switch_open_failed:
    case PairwiseFailureCode::platform_perf_context_switch_map_failed:
    case PairwiseFailureCode::platform_perf_context_switch_construction_failed:
    case PairwiseFailureCode::platform_perf_context_switch_ring_corrupt:
    case PairwiseFailureCode::platform_perf_context_switch_record_malformed:
    case PairwiseFailureCode::platform_perf_context_switch_record_capacity:
    case PairwiseFailureCode::platform_perf_context_switch_record_unsupported:
    case PairwiseFailureCode::platform_perf_context_switch_invalid_state:
    case PairwiseFailureCode::platform_perf_context_switch_capture_incomplete:
    case PairwiseFailureCode::platform_perf_context_switch_previous_drain_failed:
    case PairwiseFailureCode::platform_perf_context_switch_epoch_control_failed:
    case PairwiseFailureCode::platform_perf_context_switch_epoch_read_failed:
    case PairwiseFailureCode::platform_perf_context_switch_epoch_invalid:
    case PairwiseFailureCode::platform_perf_context_switch_epoch_loss:
    case PairwiseFailureCode::platform_perf_context_switch_unmap_failed:
    case PairwiseFailureCode::platform_perf_context_switch_close_failed:
    case PairwiseFailureCode::platform_perf_context_switch_metadata_capacity_exceeded:
    case PairwiseFailureCode::platform_hwmon_temperature_discovery_failed:
    case PairwiseFailureCode::platform_hwmon_temperature_read_failed:
    case PairwiseFailureCode::platform_hwmon_temperature_invalid_value:
    case PairwiseFailureCode::platform_hwmon_temperature_ambiguous_sensor:
    case PairwiseFailureCode::platform_hwmon_temperature_enumeration_capacity_exceeded:
    case PairwiseFailureCode::platform_hwmon_temperature_token_capacity_exceeded:
    case PairwiseFailureCode::platform_cpu_frequency_discovery_failed:
    case PairwiseFailureCode::platform_cpu_frequency_read_failed:
    case PairwiseFailureCode::platform_cpu_frequency_invalid_value:
    case PairwiseFailureCode::platform_cpu_frequency_ambiguous_policy:
    case PairwiseFailureCode::platform_cpu_frequency_enumeration_capacity_exceeded:
    case PairwiseFailureCode::platform_cpu_frequency_token_capacity_exceeded:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr auto service_placement_platform_relation(
    PairwiseFailureCode code, PairwiseFailureOperation operation) noexcept
    -> bool {
    switch (code) {
    case PairwiseFailureCode::platform_affinity_empty_request:
    case PairwiseFailureCode::platform_affinity_set_failed:
    case PairwiseFailureCode::platform_affinity_mask_allocation_failed:
        return operation == PairwiseFailureOperation::set_affinity;
    case PairwiseFailureCode::platform_affinity_conversion_failed:
        return operation == PairwiseFailureOperation::set_affinity ||
               operation == PairwiseFailureOperation::get_affinity;
    case PairwiseFailureCode::platform_affinity_get_failed:
    case PairwiseFailureCode::platform_affinity_readback_too_large:
        return operation == PairwiseFailureOperation::get_affinity;
    case PairwiseFailureCode::platform_current_cpu_query_failed:
        return operation == PairwiseFailureOperation::current_cpu;
    default:
        return false;
    }
}

[[nodiscard]] auto local_preparation_failure() noexcept -> PairwiseRuntimeFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        PairwiseFailureReason::contained_exception,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value)
        std::terminate();
    return std::move(*value);
}

[[nodiscard]] auto two_stage_runtime_failure(
    PairwiseFailureReason reason, PairwiseFailureContext context) noexcept
    -> PairwiseRuntimeFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations, reason,
        std::move(context));
    if (!value)
        std::terminate();
    return std::move(*value);
}

[[nodiscard]] auto worker_count_failure(std::size_t expected,
                                        std::size_t observed) noexcept
    -> PairwiseMonitoringPreparationFailure {
    return PairwiseMonitoringPreparationFailure::runtime(
        two_stage_runtime_failure(
            PairwiseFailureReason::worker_count_mismatch,
            ExpectedObservedCountContext{
                PairwiseCount{static_cast<std::uint64_t>(expected)},
                PairwiseCount{static_cast<std::uint64_t>(observed)}}));
}

[[nodiscard]] auto worker_association_failure(std::size_t ordinal) noexcept
    -> PairwiseMonitoringPreparationFailure {
    return PairwiseMonitoringPreparationFailure::runtime(
        two_stage_runtime_failure(
            PairwiseFailureReason::pairwise_worker_identity_mismatch,
            WorkerAssociationContext{
                PairwiseRole::first,
                PairwiseOrdinal{static_cast<std::uint64_t>(ordinal)}}));
}

[[nodiscard]] auto monitoring_storage_capacity_failure(
    std::size_t expected, std::size_t observed) noexcept
    -> PairwiseMonitoringPreparationFailure {
    return PairwiseMonitoringPreparationFailure::runtime(
        two_stage_runtime_failure(
            PairwiseFailureReason::monitoring_storage_capacity_exhausted,
            ExpectedObservedCountContext{
                PairwiseCount{static_cast<std::uint64_t>(expected)},
                PairwiseCount{static_cast<std::uint64_t>(observed)}}));
}

[[nodiscard]] auto monitoring_preparation_contained(
    PairwiseFailureReason reason) noexcept -> PairwiseMonitoringOperationFailure {
    auto value = PairwiseMonitoringFailure::create(
        PairwiseFailureCode::monitoring_linux_provider_contained_failure,
        PairwiseFailureOperation::monitoring_provider, reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::prepare});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(std::move(*value));
}

[[nodiscard]] auto monitoring_required_source_unsupported(
    monitoring::MonitoringChannel channel) noexcept
    -> PairwiseMonitoringOperationFailure {
    const auto pairwise_channel = channel == monitoring::MonitoringChannel::page_faults
        ? PairwiseMonitoringChannel::page_faults
        : channel == monitoring::MonitoringChannel::context_switches
        ? PairwiseMonitoringChannel::context_switches
        : channel == monitoring::MonitoringChannel::temperature
        ? PairwiseMonitoringChannel::temperature
        : PairwiseMonitoringChannel::frequency;
    auto value = PairwiseMonitoringFailure::create(
        PairwiseFailureCode::monitoring_required_source_unsupported,
        PairwiseFailureOperation::monitoring_driver,
        PairwiseFailureReason::none,
        MonitoringChannelContext{pairwise_channel});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(
        std::move(*value));
}

[[nodiscard]] auto monitoring_stop_contained(
    PairwiseFailureReason reason) noexcept -> PairwiseMonitoringOperationFailure {
    const auto code = reason == PairwiseFailureReason::contained_exception
        ? PairwiseFailureCode::monitoring_driver_contained_exception
        : PairwiseFailureCode::monitoring_driver_contained_unknown_failure;
    auto value = PairwiseMonitoringFailure::create(
        code, PairwiseFailureOperation::monitoring_driver, reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::stop_and_join});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(std::move(*value));
}

[[nodiscard]] auto monitoring_projection_invariant() noexcept
    -> PairwiseEvidenceFailure {
    return two_stage_runtime_failure(
        PairwiseFailureReason::contained_exception,
        LifecyclePhaseContext{PairwiseLifecyclePhase::stop_and_join});
}

[[nodiscard]] auto source_retirement_failure(
    bool frequency,
    platform_linux::detail::BoundedSysfsDisposition disposition) noexcept
    -> PairwiseMonitoringOperationFailure {
    if (disposition.kind ==
        platform_linux::detail::BoundedSysfsDispositionKind::success)
        std::terminate();
    const auto native = disposition.native_error > 0
        ? std::optional<int>{disposition.native_error}
        : std::nullopt;
    auto value = PairwisePlatformFailure::create(
        frequency
            ? PairwiseFailureCode::platform_cpu_frequency_discovery_failed
            : PairwiseFailureCode::platform_hwmon_temperature_discovery_failed,
        frequency ? PairwiseFailureOperation::cpu_frequency
                  : PairwiseFailureOperation::hwmon_temperature,
        PairwiseFailureReason::enumerate,
        LifecyclePhaseContext{
            frequency ? PairwiseLifecyclePhase::frequency_read
                      : PairwiseLifecyclePhase::temperature_read},
        native);
    if (!value)
        std::terminate();
    auto mapped = PairwiseMonitoringOperationFailure::from_platform(
        std::move(*value));
    if (!mapped)
        std::terminate();
    return std::move(*mapped);
}

[[nodiscard]] auto monitoring_active_capture_stop_failure() noexcept
    -> PairwiseMonitoringOperationFailure {
    auto value = PairwiseMonitoringFailure::create(
        PairwiseFailureCode::monitoring_invalid_state,
        PairwiseFailureOperation::monitoring_driver,
        PairwiseFailureReason::stop_join_active_capture,
        LifecyclePhaseContext{PairwiseLifecyclePhase::stop_and_join});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(
        std::move(*value));
}

[[nodiscard]] auto monitoring_page_classifier_failure(
    monitoring::detail::PageFaultClassifierFailure failure) noexcept
    -> PairwiseMonitoringOperationFailure {
    auto code = PairwiseFailureCode::monitoring_page_faults_invalid_input;
    auto reason = PairwiseFailureReason::empty_workers;
    switch (failure) {
    case monitoring::detail::PageFaultClassifierFailure::empty_workers:
        break;
    case monitoring::detail::PageFaultClassifierFailure::duplicate_worker:
        reason = PairwiseFailureReason::duplicate_worker;
        break;
    case monitoring::detail::PageFaultClassifierFailure::worker_scope:
        code = PairwiseFailureCode::monitoring_page_faults_association_mismatch;
        reason = PairwiseFailureReason::worker_scope;
        break;
    case monitoring::detail::PageFaultClassifierFailure::acquisition_metadata:
        code = PairwiseFailureCode::monitoring_page_faults_association_mismatch;
        reason = PairwiseFailureReason::acquisition_metadata;
        break;
    case monitoring::detail::PageFaultClassifierFailure::reversed_observation_interval:
        reason = PairwiseFailureReason::reversed_observation_interval;
        break;
    case monitoring::detail::PageFaultClassifierFailure::before_overlaps_observation:
        code = PairwiseFailureCode::monitoring_page_faults_invalid_bracket;
        reason = PairwiseFailureReason::before_overlaps_observation;
        break;
    case monitoring::detail::PageFaultClassifierFailure::after_overlaps_observation:
        code = PairwiseFailureCode::monitoring_page_faults_invalid_bracket;
        reason = PairwiseFailureReason::after_overlaps_observation;
        break;
    case monitoring::detail::PageFaultClassifierFailure::cumulative_counter_decreased:
        code = PairwiseFailureCode::monitoring_page_faults_counter_decreased;
        reason = PairwiseFailureReason::cumulative_counter_decreased;
        break;
    }
    auto value = PairwiseMonitoringFailure::create(
        code, PairwiseFailureOperation::classify_page_faults, reason,
        MonitoringChannelContext{PairwiseMonitoringChannel::page_faults});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(
        std::move(*value));
}

[[nodiscard]] auto monitoring_context_classifier_failure(
    monitoring::detail::ContextSwitchClassifierFailure failure) noexcept
    -> PairwiseMonitoringOperationFailure {
    using Failure = monitoring::detail::ContextSwitchClassifierFailure;
    auto code = PairwiseFailureCode::monitoring_context_switches_invalid_input;
    auto reason = PairwiseFailureReason::empty_workers;
    switch (failure) {
    case Failure::empty_workers: break;
    case Failure::duplicate_worker:
        reason = PairwiseFailureReason::duplicate_worker;
        break;
    case Failure::reversed_observation_interval:
        reason = PairwiseFailureReason::reversed_observation_interval;
        break;
    case Failure::worker_view_count:
        code = PairwiseFailureCode::monitoring_context_switches_association_mismatch;
        reason = PairwiseFailureReason::worker_view_count;
        break;
    case Failure::worker_source:
        code = PairwiseFailureCode::monitoring_context_switches_association_mismatch;
        reason = PairwiseFailureReason::worker_source;
        break;
    case Failure::acquisition_metadata:
        code = PairwiseFailureCode::monitoring_context_switches_association_mismatch;
        reason = PairwiseFailureReason::acquisition_metadata;
        break;
    case Failure::batch_worker:
        code = PairwiseFailureCode::monitoring_context_switches_association_mismatch;
        reason = PairwiseFailureReason::batch_worker;
        break;
    case Failure::event_worker:
        code = PairwiseFailureCode::monitoring_context_switches_association_mismatch;
        reason = PairwiseFailureReason::event_worker;
        break;
    case Failure::interval_coverage:
        code = PairwiseFailureCode::monitoring_context_switches_capture_incomplete;
        reason = PairwiseFailureReason::interval_coverage;
        break;
    case Failure::lost_records:
        code = PairwiseFailureCode::monitoring_context_switches_capture_incomplete;
        reason = PairwiseFailureReason::lost_records;
        break;
    case Failure::event_count:
        code = PairwiseFailureCode::monitoring_context_switches_count_overflow;
        reason = PairwiseFailureReason::event_count;
        break;
    }
    auto value = PairwiseMonitoringFailure::create(
        code, PairwiseFailureOperation::classify_context_switches, reason,
        MonitoringChannelContext{PairwiseMonitoringChannel::context_switches});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(std::move(*value));
}

[[nodiscard]] auto monitoring_temperature_classifier_failure(
    monitoring::detail::TemperatureClassifierFailure failure) noexcept
    -> PairwiseMonitoringOperationFailure {
    using Failure = monitoring::detail::TemperatureClassifierFailure;
    auto code = PairwiseFailureCode::monitoring_temperature_invalid_input;
    auto reason = PairwiseFailureReason::ordering;
    switch (failure) {
    case Failure::ordering: break;
    case Failure::interval: reason = PairwiseFailureReason::interval; break;
    case Failure::boundary_overlap:
        reason = PairwiseFailureReason::boundary_overlap;
        break;
    case Failure::metadata:
        code = PairwiseFailureCode::monitoring_temperature_association_mismatch;
        reason = PairwiseFailureReason::metadata;
        break;
    case Failure::sensor:
        code = PairwiseFailureCode::monitoring_temperature_association_mismatch;
        reason = PairwiseFailureReason::sensor;
        break;
    case Failure::driver:
        code = PairwiseFailureCode::monitoring_temperature_capture_incomplete;
        reason = PairwiseFailureReason::driver;
        break;
    case Failure::sample_count:
        code = PairwiseFailureCode::monitoring_temperature_capture_incomplete;
        reason = PairwiseFailureReason::sample_count;
        break;
    case Failure::leading_gap:
        code = PairwiseFailureCode::monitoring_temperature_capture_incomplete;
        reason = PairwiseFailureReason::leading_gap;
        break;
    case Failure::internal_gap:
        code = PairwiseFailureCode::monitoring_temperature_capture_incomplete;
        reason = PairwiseFailureReason::internal_gap;
        break;
    case Failure::trailing_gap:
        code = PairwiseFailureCode::monitoring_temperature_capture_incomplete;
        reason = PairwiseFailureReason::trailing_gap;
        break;
    }
    auto value = PairwiseMonitoringFailure::create(
        code, PairwiseFailureOperation::classify_temperature, reason,
        MonitoringChannelContext{PairwiseMonitoringChannel::temperature});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(
        std::move(*value));
}

[[nodiscard]] auto monitoring_frequency_classifier_failure(
    monitoring::detail::FrequencyClassifierFailure failure) noexcept
    -> PairwiseMonitoringOperationFailure {
    using Failure = monitoring::detail::FrequencyClassifierFailure;
    auto code = PairwiseFailureCode::monitoring_frequency_invalid_input;
    auto reason = PairwiseFailureReason::ordering;
    switch (failure) {
    case Failure::ordering: break;
    case Failure::interval: reason = PairwiseFailureReason::interval; break;
    case Failure::boundary_overlap:
        reason = PairwiseFailureReason::boundary_overlap;
        break;
    case Failure::ratio: reason = PairwiseFailureReason::ratio; break;
    case Failure::domain_order:
        reason = PairwiseFailureReason::domain_order;
        break;
    case Failure::domain: reason = PairwiseFailureReason::domain; break;
    case Failure::metadata:
        code = PairwiseFailureCode::monitoring_frequency_association_mismatch;
        reason = PairwiseFailureReason::metadata;
        break;
    case Failure::implementation:
        code = PairwiseFailureCode::monitoring_frequency_association_mismatch;
        reason = PairwiseFailureReason::implementation;
        break;
    case Failure::source:
        code = PairwiseFailureCode::monitoring_frequency_association_mismatch;
        reason = PairwiseFailureReason::source;
        break;
    case Failure::driver:
        code = PairwiseFailureCode::monitoring_frequency_capture_incomplete;
        reason = PairwiseFailureReason::driver;
        break;
    case Failure::sample_count:
        code = PairwiseFailureCode::monitoring_frequency_capture_incomplete;
        reason = PairwiseFailureReason::sample_count;
        break;
    case Failure::leading_gap:
        code = PairwiseFailureCode::monitoring_frequency_capture_incomplete;
        reason = PairwiseFailureReason::leading_gap;
        break;
    case Failure::internal_gap:
        code = PairwiseFailureCode::monitoring_frequency_capture_incomplete;
        reason = PairwiseFailureReason::internal_gap;
        break;
    case Failure::trailing_gap:
        code = PairwiseFailureCode::monitoring_frequency_capture_incomplete;
        reason = PairwiseFailureReason::trailing_gap;
        break;
    }
    auto value = PairwiseMonitoringFailure::create(
        code, PairwiseFailureOperation::classify_frequency, reason,
        MonitoringChannelContext{PairwiseMonitoringChannel::frequency});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(
        std::move(*value));
}

[[nodiscard]] auto checked_size(std::uint64_t value) noexcept
    -> std::optional<std::size_t> {
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()))
        return std::nullopt;
    return static_cast<std::size_t>(value);
}

[[nodiscard]] auto checked_add_size(std::size_t left, std::size_t right) noexcept
    -> std::optional<std::size_t> {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        return std::nullopt;
    return left + right;
}

[[nodiscard]] auto checked_product_size(std::size_t left,
                                        std::size_t right) noexcept
    -> std::optional<std::size_t> {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
        return std::nullopt;
    return left * right;
}

[[nodiscard]] auto monitoring_rollback_cleanup_capacity(
    bool temperature_requested, bool frequency_requested,
    std::size_t domains, std::size_t rollback_values) noexcept
    -> std::optional<std::size_t> {
    if (rollback_values == 0U)
        return std::nullopt;
    const auto frequency_pairs = frequency_requested
        ? checked_product_size(2U, domains)
        : std::optional<std::size_t>{0U};
    const auto frequency_cleanup = frequency_pairs
        ? checked_add_size(*frequency_pairs, frequency_requested ? 1U : 0U)
        : std::nullopt;
    const auto source_cleanup = frequency_cleanup
        ? checked_add_size(temperature_requested ? 3U : 0U,
                           *frequency_cleanup)
        : std::nullopt;
    return source_cleanup
        ? checked_add_size(rollback_values - 1U, *source_cleanup)
        : std::nullopt;
}

[[nodiscard]] auto monitoring_preparation_failure(
    PairwiseSysfsPreparationFailure failure) noexcept
    -> PairwiseMonitoringPreparationFailure {
    try {
        auto primary = PairwiseMonitoringOperationFailure::from_platform(
            std::move(failure.primary));
        if (!primary)
            return PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure());
        auto cleanup = std::vector<PairwiseMonitoringOperationFailure>{};
        cleanup.reserve(failure.cleanup.size());
        for (auto& value : failure.cleanup) {
            auto mapped = PairwiseMonitoringOperationFailure::from_platform(
                std::move(value));
            if (!mapped)
                return PairwiseMonitoringPreparationFailure::runtime(
                    local_preparation_failure());
            cleanup.push_back(std::move(*mapped));
        }
        return PairwiseMonitoringPreparationFailure::operation(
            std::move(*primary), std::move(cleanup));
    } catch (...) {
        return PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure());
    }
}

} // namespace

enum class PairwiseMonitoringCaptureRegistrationState : std::uint8_t {
    unregistered,
    active,
    retired,
};

class PairwiseMonitoringCaptureRegistration final {
  public:
    [[nodiscard]] auto activate(std::uint64_t generation) noexcept -> bool {
        generation_.store(generation, std::memory_order_relaxed);
        auto expected = PairwiseMonitoringCaptureRegistrationState::unregistered;
        return state_.compare_exchange_strong(
            expected, PairwiseMonitoringCaptureRegistrationState::active,
            std::memory_order_release, std::memory_order_relaxed);
    }
    [[nodiscard]] auto active(std::uint64_t generation) const noexcept -> bool {
        if (state_.load(std::memory_order_acquire) !=
            PairwiseMonitoringCaptureRegistrationState::active)
            return false;
        return generation_.load(std::memory_order_relaxed) == generation;
    }
    [[nodiscard]] auto retire(std::uint64_t generation) noexcept -> bool {
        if (generation_.load(std::memory_order_relaxed) != generation)
            return false;
        auto expected = PairwiseMonitoringCaptureRegistrationState::active;
        return state_.compare_exchange_strong(
            expected, PairwiseMonitoringCaptureRegistrationState::retired,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
    [[nodiscard]] auto unregister(std::uint64_t generation) noexcept -> bool {
        if (generation_.load(std::memory_order_relaxed) != generation)
            return false;
        auto current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (current != PairwiseMonitoringCaptureRegistrationState::active &&
                current != PairwiseMonitoringCaptureRegistrationState::retired)
                return false;
            if (state_.compare_exchange_weak(
                    current,
                    PairwiseMonitoringCaptureRegistrationState::unregistered,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }
    [[nodiscard]] auto state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
  private:
    std::atomic<PairwiseMonitoringCaptureRegistrationState> state_{
        PairwiseMonitoringCaptureRegistrationState::unregistered};
    std::atomic<std::uint64_t> generation_{};
};

struct PairwiseMonitoringResetFailureSequence final {
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> values;
    std::size_t count{};
};
using PairwiseMonitoringResetFailure =
    std::variant<PairwiseMonitoringResetFailureSequence,
                 PairwiseMonitoringCaptureFailure>;

struct PairwisePreparedFrequencyDomain final {
    std::span<core::LinuxCpuId> cpus;
    monitoring::FrequencySourceKind kind{};
    std::string driver;
    std::string governor;
    std::uint64_t minimum_hz{};
    std::uint64_t maximum_hz{};
};

struct PairwiseMonitoringOwningFacade::State final {
    explicit State(PairwiseAdmittedMonitoringCapacity value) noexcept
        : capacity{std::move(value)} {}

    PairwiseAdmittedMonitoringCapacity capacity;
    std::vector<monitoring::MonitoringWorkerScope> workers;
    std::optional<monitoring::detail::PairwisePreparedCaptureSlots> capture_slots;
    std::optional<monitoring::detail::PairwisePreparedClassifierScratch>
        classifier_scratch;
    std::vector<PairwiseTaskStatWorkspace> page_workspaces;
    std::vector<PairwisePreparedPerfContextSwitch> context_sources;
    std::vector<PairwisePerfDrainStorage> context_drain_storage;
    std::vector<PairwisePreparedFrequencyDomain> frequency_metadata;
    std::vector<core::LinuxCpuId> frequency_cpu_storage;
    std::size_t frequency_domain_count{};
    bool frequency_partition_committed{};
    std::optional<PairwiseSysfsReadWorkspace> temperature_workspace;
    std::vector<PairwiseSysfsReadWorkspace> frequency_workspaces;
    std::optional<PairwisePreparedTemperatureSource> temperature_source;
    std::optional<PairwisePreparedFrequencySources> frequency_sources;
    std::optional<PairwisePlatformLeafFacade> retirement_platform;
    std::vector<platform_linux::detail::BoundedSysfsDisposition>
        source_retirement;
    std::size_t source_retirement_count{};
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> rollback_returned;
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> completion_returned;
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> stop_returned;
    std::vector<PairwiseMonitoringPageReadDisposition> page_read_dispositions;
    std::optional<monitoring::detail::PairwisePreparedCaptureSlot> active_slot;
    std::shared_ptr<PairwiseMonitoringCaptureRegistration[]> capture_registrations;
    std::size_t capture_registration_count{};
    std::optional<std::size_t> active_registration;
    [[nodiscard]] auto active_registration_ptr() noexcept
        -> PairwiseMonitoringCaptureRegistration* {
        return active_registration && capture_registrations
            ? capture_registrations.get() + *active_registration
            : nullptr;
    }
    std::size_t next_capture_slot{};
    std::uint64_t generation{};
    bool capture_active{};
    bool capture_sources_terminal{};
    bool service_terminal{};
    bool stopped{};
    MonitoringPreparationCheckpoint preparation_checkpoint;
    struct AsyncState final {
        AsyncState() noexcept(false) : stop{} {}
        std::stop_source stop;
        std::atomic<bool> wake{};
        std::mutex mutex;
        std::condition_variable condition;
        std::mutex scheduler_mutex;
        std::condition_variable scheduler_condition;
        bool scheduler_wake{};
        bool lifetime_stop{};
        bool arm_requested{};
        bool finish_requested{};
        bool abort_requested{};
        bool armed{};
        bool quiesced{true};
        bool publication_enabled{};
        bool round_in_flight{};
        std::uint64_t generation{};
        std::size_t next_ordinal{};
        platform_linux::RawMonotonicTimestamp origin =
            platform_linux::RawMonotonicTimestamp::from_nanoseconds(0U);
        std::optional<PairwiseMonitoringCaptureFailure> terminal_failure;
        std::optional<PairwiseMonitoringResetFailureSequence> reset_failures;
        std::optional<core::LinuxCpuId> service_cpu;
        std::optional<PairwisePreparedAffinityWorkspace> affinity_workspace;
        bool startup_ready{};
        std::optional<std::variant<PairwiseRuntimeFailure,
                                   PairwiseMonitoringOperationFailure>>
            startup_failure;
        bool missing_placement_operation{};
        std::optional<std::jthread> service;
        std::array<PairwiseMonitoringForceDisposition, 3> force{};
        std::size_t force_count{};
        std::array<PairwiseMonitoringServiceJoinFault, 3> join_script{};
        std::size_t join_script_count{};
        std::size_t join_script_position{};
        PairwiseMonitoringServiceRunFault run_fault{};
    };
    std::optional<AsyncState> async;
    bool service_owner{};
    bool temperature_supported{};
    bool frequency_supported{};
    bool temperature_unavailable{};
    bool frequency_unavailable{};
    std::vector<platform_linux::RawMonotonicTimestamp> context_coverage_start;
    std::vector<platform_linux::RawMonotonicTimestamp> context_coverage_end;
    std::vector<monitoring::ContextSwitchCaptureCompleteness>
        context_completeness;
};
// Compile-time baseline only: mirrors every pre-checkpoint State data member.
// The source audit compares this inventory mechanically; no baseline object exists.
namespace {
struct MonitoringPreparationPreviousStateLayout final {
    PairwiseAdmittedMonitoringCapacity capacity;
    std::vector<monitoring::MonitoringWorkerScope> workers;
    std::optional<monitoring::detail::PairwisePreparedCaptureSlots> capture_slots;
    std::optional<monitoring::detail::PairwisePreparedClassifierScratch>
        classifier_scratch;
    std::vector<PairwiseTaskStatWorkspace> page_workspaces;
    std::vector<PairwisePreparedPerfContextSwitch> context_sources;
    std::vector<PairwisePerfDrainStorage> context_drain_storage;
    std::vector<PairwisePreparedFrequencyDomain> frequency_metadata;
    std::vector<core::LinuxCpuId> frequency_cpu_storage;
    std::size_t frequency_domain_count{};
    bool frequency_partition_committed{};
    std::optional<PairwiseSysfsReadWorkspace> temperature_workspace;
    std::vector<PairwiseSysfsReadWorkspace> frequency_workspaces;
    std::optional<PairwisePreparedTemperatureSource> temperature_source;
    std::optional<PairwisePreparedFrequencySources> frequency_sources;
    std::optional<PairwisePlatformLeafFacade> retirement_platform;
    std::vector<platform_linux::detail::BoundedSysfsDisposition>
        source_retirement;
    std::size_t source_retirement_count{};
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> rollback_returned;
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> completion_returned;
    std::vector<std::optional<PairwiseMonitoringOperationFailure>> stop_returned;
    std::vector<PairwiseMonitoringPageReadDisposition> page_read_dispositions;
    std::optional<monitoring::detail::PairwisePreparedCaptureSlot> active_slot;
    std::shared_ptr<PairwiseMonitoringCaptureRegistration[]> capture_registrations;
    std::size_t capture_registration_count{};
    std::optional<std::size_t> active_registration;
    std::size_t next_capture_slot{};
    std::uint64_t generation{};
    bool capture_active{};
    bool capture_sources_terminal{};
    bool service_terminal{};
    bool stopped{};
    std::optional<PairwiseMonitoringOwningFacade::State::AsyncState> async;
    bool service_owner{};
    bool temperature_supported{};
    bool frequency_supported{};
    bool temperature_unavailable{};
    bool frequency_unavailable{};
    std::vector<platform_linux::RawMonotonicTimestamp> context_coverage_start;
    std::vector<platform_linux::RawMonotonicTimestamp> context_coverage_end;
    std::vector<monitoring::ContextSwitchCaptureCompleteness>
        context_completeness;
};
constexpr auto monitoring_checkpoint_alignment = alignof(MonitoringPreparationCheckpoint);
constexpr auto monitoring_checkpoint_storage =
    (sizeof(MonitoringPreparationCheckpoint) + monitoring_checkpoint_alignment - 1U) /
    monitoring_checkpoint_alignment * monitoring_checkpoint_alignment;
static_assert(alignof(PairwiseMonitoringOwningFacade::State) ==
              alignof(MonitoringPreparationPreviousStateLayout));
static_assert(sizeof(PairwiseMonitoringOwningFacade::State) ==
              sizeof(MonitoringPreparationPreviousStateLayout) + monitoring_checkpoint_storage);
} // namespace


struct PairwisePreparedMonitoringStorage::State final {
    std::unique_ptr<PairwiseMonitoringOwningFacade::State> storage;
    std::optional<PairwisePreparedHwmonDiscoveryStorage> temperature_discovery;
    std::optional<PairwisePreparedCpuFrequencyDiscoveryStorage>
        frequency_discovery;
    std::vector<PairwisePerfPreparationWorkspace> context_workspaces;
    std::optional<PairwisePreparedAffinityWorkspace> service_affinity_workspace;
    std::vector<PairwiseMonitoringOperationFailure> rollback_cleanup;
    bool bound{};
    bool consumed{};
};

namespace {

[[nodiscard]] auto terminalize_monitoring_service(
    PairwiseMonitoringOwningFacade::State& state) noexcept
    -> PairwiseMonitoringServiceJoinFault {
    auto& async = *state.async;
    async.force_count = 0U;
    const auto join_once = [&]() noexcept {
        auto scripted = PairwiseMonitoringServiceJoinFault::success;
        if (async.join_script_position < async.join_script_count)
            scripted = async.join_script[async.join_script_position++];
        if (scripted != PairwiseMonitoringServiceJoinFault::success)
            return scripted;
        try {
            async.service->join();
            return PairwiseMonitoringServiceJoinFault::success;
        } catch (const std::system_error&) {
            return PairwiseMonitoringServiceJoinFault::native_failure;
        } catch (const std::exception&) {
            return PairwiseMonitoringServiceJoinFault::contained_standard;
        } catch (...) {
            return PairwiseMonitoringServiceJoinFault::contained_unknown;
        }
    };
    const auto ordinary = join_once();
    for (std::uint8_t attempt = 1U;
         async.service->joinable() && attempt <= 2U; ++attempt) {
        auto& disposition = async.force[async.force_count++];
        disposition.attempt = attempt;
        const auto forced = join_once();
        disposition.outcome = static_cast<std::uint8_t>(forced);
        disposition.ownership_remains = async.service->joinable();
    }
    if (async.service->joinable()) {
        auto& final = async.force[async.force_count++];
        final.attempt = 3U;
        final.outcome = static_cast<std::uint8_t>(
            PairwiseMonitoringServiceJoinFault::contained_unknown);
        final.ownership_remains = true;
        std::terminate();
    }
    async.service.reset();
    state.service_owner = false;
    return ordinary;
}

[[nodiscard]] auto monitoring_driver_transition_failure(
    PairwiseFailureCode code, PairwiseFailureReason reason,
    PairwiseLifecyclePhase phase) noexcept
    -> PairwiseMonitoringCaptureFailure {
    auto value = PairwiseMonitoringFailure::create(
        code, PairwiseFailureOperation::monitoring_driver, reason,
        code == PairwiseFailureCode::monitoring_driver_invariant_failed
            ? PairwiseFailureContext{NoPairwiseFailureContext{}}
            : PairwiseFailureContext{LifecyclePhaseContext{phase}});
    if (!value)
        std::terminate();
    return PairwiseMonitoringCaptureFailure{
        PairwiseMonitoringOperationFailure::from_monitoring(
            std::move(*value))};
}

[[nodiscard]] auto monitoring_missing_service_binding_failure() noexcept
    -> PairwiseMonitoringOperationFailure {
    auto value = PairwiseMonitoringFailure::create(
        PairwiseFailureCode::monitoring_service_start_failed,
        PairwiseFailureOperation::monitoring_driver,
        PairwiseFailureReason::missing_operation,
        MissingOperationContext{PairwiseMissingOperation::bind_service_cpu});
    if (!value)
        std::terminate();
    return PairwiseMonitoringOperationFailure::from_monitoring(
        std::move(*value));
}

[[nodiscard]] auto scheduler_failure(
    PairwisePlatformFailure failure) noexcept
    -> PairwiseMonitoringCaptureFailure {
    auto value = PairwiseMonitoringSchedulerFailure::from_platform(
        std::move(failure));
    if (!value)
        std::terminate();
    return PairwiseMonitoringCaptureFailure{std::move(*value)};
}

[[nodiscard]] auto operation_failure(
    PairwisePlatformFailure failure,
    std::optional<PairwiseMonitoringPageReadDisposition> disposition =
        std::nullopt) noexcept -> PairwiseMonitoringCaptureFailure {
    auto value = PairwiseMonitoringOperationFailure::from_platform(
        std::move(failure));
    if (!value)
        std::terminate();
    return PairwiseMonitoringCaptureFailure{std::move(*value), disposition};
}

[[nodiscard]] auto read_page_round(
    PairwiseMonitoringOwningFacade::State& state,
    monitoring::detail::PairwisePreparedCaptureSlot& slot,
    const PairwisePlatformLeafFacade& platform, bool before,
    std::size_t capture_index) noexcept
    -> std::expected<void, PairwiseMonitoringCaptureFailure> {
    const auto workers = state.workers.size();
    const auto capture_stride = workers * 2U;
    const auto base = capture_index * capture_stride + (before ? 0U : workers);
    for (std::size_t index{}; index < workers; ++index) {
        auto started = platform.raw_monotonic_now();
        if (!started)
            return std::unexpected{scheduler_failure(
                std::move(started.error()))};
        auto& disposition = state.page_read_dispositions[base + index];
        disposition.attempted = true;
        auto counters = platform.read_task_page_fault_counters(
            state.workers[index].operational_thread_id(),
            state.page_workspaces[index]);
        if (!counters) {
            std::optional<PairwiseMonitoringPageReadDisposition> secondary;
            if (counters.error().cleanup) {
                disposition.close_failed = true;
                disposition.native_error =
                    counters.error().cleanup->posix_errno().value_or(0);
                secondary = disposition;
            } else if (counters.error().primary.code() ==
                           PairwiseFailureCode::platform_task_stat_read_failed &&
                       counters.error().primary.reason() ==
                           PairwiseFailureReason::close_failed) {
                disposition.close_failed = true;
                disposition.native_error =
                    counters.error().primary.posix_errno().value_or(0);
                secondary = disposition;
            }
            return std::unexpected{operation_failure(
                std::move(counters.error().primary), secondary)};
        }
        auto completed = platform.raw_monotonic_now();
        if (!completed)
            return std::unexpected{scheduler_failure(
                std::move(completed.error()))};
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                append_page_acquisition(
                    slot, before, state.workers[index].worker_id(), *started,
                    *completed, *counters))
            std::terminate();
    }
    return {};
}

[[nodiscard]] auto monitoring_schedule_capacity_failure() noexcept
    -> PairwiseMonitoringCaptureFailure {
    auto value = PairwiseMonitoringFailure::create(
        PairwiseFailureCode::monitoring_scheduled_capacity_exceeded,
        PairwiseFailureOperation::monitoring_driver,
        PairwiseFailureReason::scheduled_capacity_exceeded,
        NoPairwiseFailureContext{});
    if (!value)
        std::terminate();
    return PairwiseMonitoringCaptureFailure{
        PairwiseMonitoringOperationFailure::from_monitoring(std::move(*value))};
}

[[nodiscard]] auto monitoring_deadline_failure() noexcept
    -> PairwiseMonitoringCaptureFailure {
    auto value = PairwiseMonitoringFailure::create(
        PairwiseFailureCode::monitoring_driver_invariant_failed,
        PairwiseFailureOperation::monitoring_driver,
        PairwiseFailureReason::deadline_overflow,
        NoPairwiseFailureContext{});
    if (!value)
        std::terminate();
    return PairwiseMonitoringCaptureFailure{
        PairwiseMonitoringOperationFailure::from_monitoring(std::move(*value))};
}

[[nodiscard]] auto monitoring_service_contained_failure(bool unknown) noexcept
    -> PairwiseMonitoringCaptureFailure {
    auto value = PairwiseMonitoringFailure::create(
        unknown
            ? PairwiseFailureCode::monitoring_driver_contained_unknown_failure
            : PairwiseFailureCode::monitoring_driver_contained_exception,
        PairwiseFailureOperation::monitoring_driver,
        unknown ? PairwiseFailureReason::contained_unknown_failure
                : PairwiseFailureReason::contained_exception,
        LifecyclePhaseContext{PairwiseLifecyclePhase::service_exception});
    if (!value)
        std::terminate();
    return PairwiseMonitoringCaptureFailure{
        PairwiseMonitoringOperationFailure::from_monitoring(std::move(*value))};
}

[[nodiscard]] auto disable_context_sources(
    PairwiseMonitoringOwningFacade::State& state) noexcept
    -> std::expected<void, PairwiseMonitoringResetFailureSequence> {
    for (auto& value : state.completion_returned) value.reset();
    std::size_t count{};
    for (auto& source : state.context_sources) {
        auto disabled = state.retirement_platform->disable_perf_context_switch_capture(source);
        if (!disabled) {
            auto failure = PairwiseMonitoringOperationFailure::from_platform(std::move(disabled.error()));
            if (!failure || count >= state.completion_returned.size()) std::terminate();
            state.completion_returned[count++].emplace(std::move(*failure));
        }
    }
    if (count) {
        state.capture_sources_terminal = true;
        return std::unexpected{PairwiseMonitoringResetFailureSequence{
            std::move(state.completion_returned), count}};
    }
    return {};
}

[[nodiscard]] auto reset_context_sources(
    PairwiseMonitoringOwningFacade::State& state,
    bool establish_boundary = true) noexcept
    -> std::expected<platform_linux::RawMonotonicTimestamp,
                     PairwiseMonitoringResetFailure> {
    auto& platform = *state.retirement_platform;
    if (!establish_boundary) {
        // An aborted context epoch is never reset into a reusable source.
        if (!state.context_sources.empty()) state.capture_sources_terminal = true;
        auto disabled = disable_context_sources(state);
        if (!disabled) return std::unexpected{PairwiseMonitoringResetFailure{
            std::move(disabled.error())}};
        return platform_linux::RawMonotonicTimestamp::from_nanoseconds(0U);
    }
    const auto failed = [&](PairwiseMonitoringCaptureFailure primary)
        -> std::expected<platform_linux::RawMonotonicTimestamp, PairwiseMonitoringResetFailure> {
        if (!state.context_sources.empty()) state.capture_sources_terminal = true;
        auto disabled = disable_context_sources(state);
        if (!disabled) return std::unexpected{PairwiseMonitoringResetFailure{
            PairwiseMonitoringCaptureFailure::with_cleanup(std::move(primary),
                std::move(disabled.error().values), disabled.error().count)}};
        return std::unexpected{PairwiseMonitoringResetFailure{std::move(primary)}};
    };
    for (std::size_t index{}; index < state.context_sources.size(); ++index) {
        state.context_drain_storage[index].reset_drain_boundary();
        if (state.active_slot)
            monitoring::detail::PairwisePreparedCaptureStorageAccess::increment_context_drain(*state.active_slot);
        auto reset = platform.reset_perf_context_switch_prepared(
            state.context_sources[index], state.context_drain_storage[index]);
        if (!reset) return failed(operation_failure(std::move(reset.error())));
        auto enabled = platform.begin_perf_context_switch_capture(state.context_sources[index]);
        if (!enabled) return failed(operation_failure(std::move(enabled.error())));
    }
    // Common coverage starts only once every prepared source is enabled.
    auto origin = platform.raw_monotonic_now();
    if (!origin) return failed(scheduler_failure(std::move(origin.error())));
    std::ranges::fill(state.context_coverage_start, *origin);
    std::ranges::fill(state.context_coverage_end, *origin);
    std::ranges::fill(state.context_completeness,
                      monitoring::ContextSwitchCaptureCompleteness::complete);
    return *origin;
}

[[nodiscard]] auto reset_sequence_primary(
    PairwiseMonitoringResetFailureSequence sequence) noexcept
    -> PairwiseMonitoringCaptureFailure {
    if (sequence.count == 0U || !sequence.values[0U])
        std::terminate();
    auto primary = PairwiseMonitoringAcquisitionFailure{
        std::move(*sequence.values[0U])};
    return PairwiseMonitoringCaptureFailure{
        std::move(primary), std::nullopt, std::move(sequence.values), 1U,
        sequence.count - 1U};
}

[[nodiscard]] auto reset_failure_primary(
    PairwiseMonitoringResetFailure failure) noexcept
    -> PairwiseMonitoringCaptureFailure {
    if (auto* sequence =
            std::get_if<PairwiseMonitoringResetFailureSequence>(&failure))
        return reset_sequence_primary(std::move(*sequence));
    return std::move(std::get<PairwiseMonitoringCaptureFailure>(failure));
}

[[nodiscard]] auto append_reset_cleanup(
    PairwiseMonitoringCaptureFailure primary,
    PairwiseMonitoringResetFailureSequence sequence) noexcept
    -> PairwiseMonitoringCaptureFailure {
    return PairwiseMonitoringCaptureFailure::with_cleanup(
        std::move(primary), std::move(sequence.values), sequence.count);
}

[[nodiscard]] auto close_context_epoch(PairwiseMonitoringOwningFacade::State& state) noexcept
    -> std::expected<void, PairwiseMonitoringCaptureFailure> {
    if (state.context_sources.empty()) return {};
    // finish_requested follows worker quiescence. Timestamp before the first
    // disable bounds the common enabled interval; later drain clocks do not.
    auto end = state.retirement_platform->raw_monotonic_now();
    if (end) std::ranges::fill(state.context_coverage_end, *end);
    auto disabled = disable_context_sources(state);
    if (!end) {
        state.capture_sources_terminal = true;
        auto primary = scheduler_failure(std::move(end.error()));
        if (!disabled) return std::unexpected{append_reset_cleanup(
            std::move(primary), std::move(disabled.error()))};
        return std::unexpected{std::move(primary)};
    }
    if (!disabled) return std::unexpected{reset_sequence_primary(std::move(disabled.error()))};
    return {};
}

[[nodiscard]] auto verify_context_epoch_loss(PairwiseMonitoringOwningFacade::State& state) noexcept
    -> std::expected<void, PairwiseMonitoringCaptureFailure> {
    for (auto& value : state.completion_returned) value.reset();
    std::size_t count{};
    for (auto& source : state.context_sources) {
        auto verified = state.retirement_platform->verify_perf_context_switch_capture_loss(source);
        if (!verified) {
            auto failure = PairwiseMonitoringOperationFailure::from_platform(std::move(verified.error()));
            if (!failure || count >= state.completion_returned.size()) std::terminate();
            state.completion_returned[count++].emplace(std::move(*failure));
        }
    }
    if (count) {
        state.capture_sources_terminal = true;
        return std::unexpected{reset_sequence_primary(PairwiseMonitoringResetFailureSequence{
            std::move(state.completion_returned), count})};
    }
    return {};
}

[[nodiscard]] auto read_context_round(
    PairwiseMonitoringOwningFacade::State& state, std::size_t ordinal,
    PairwisePerfDrainRole role) noexcept
    -> std::expected<void, PairwiseMonitoringCaptureFailure> {
    if (!state.active_slot)
        std::terminate();
    auto& platform = *state.retirement_platform;
    const auto rounds = static_cast<std::size_t>(state.capacity.round_capacity());
    for (std::size_t index{}; index < state.context_sources.size(); ++index) {
        monitoring::detail::PairwisePreparedCaptureStorageAccess::
            increment_context_drain(*state.active_slot);
        auto started = platform.raw_monotonic_now();
        if (!started)
            return std::unexpected{scheduler_failure(std::move(started.error()))};
        auto drained = platform.drain_perf_context_switch_prepared(
            state.context_sources[index], state.context_drain_storage[index]);
        if (!drained) {
            if (const auto* original =
                    std::get_if<PairwisePerfLossContext>(&drained.error().context())) {
                auto context = *original;
                context.drain = {role, PairwiseOrdinal{ordinal},
                                 PairwiseCount{started->nanoseconds()}};
                if (const auto previous = state.context_drain_storage[index].
                        last_successful_drain_completed())
                    context.last_successful_drain_completed_ns =
                        PairwiseCount{previous->nanoseconds()};
                auto enriched = PairwisePlatformFailure::create(
                    drained.error().code(), drained.error().operation(),
                    drained.error().reason(), context, drained.error().posix_errno());
                if (enriched)
                    return std::unexpected{operation_failure(std::move(*enriched))};
            }
            return std::unexpected{operation_failure(std::move(drained.error()))};
        }
        auto completed = platform.raw_monotonic_now();
        if (!completed)
            return std::unexpected{scheduler_failure(std::move(completed.error()))};
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                append_context_acquisition(
                    *state.active_slot, index, ordinal, rounds,
                    state.workers[index].worker_id(), *started, *completed,
                    drained->records, drained->completeness))
            std::terminate();
        // The final drain occurs after disable and cannot extend coverage.
        if (role != PairwisePerfDrainRole::final)
            state.context_coverage_end[index] = *started;
        state.context_drain_storage[index].note_drain_completed(*completed);
        if (drained->completeness ==
            platform_linux::PerfCaptureCompleteness::incomplete)
            state.context_completeness[index] =
                monitoring::ContextSwitchCaptureCompleteness::incomplete;
    }
    return {};
}

[[nodiscard]] auto read_point_round(
    PairwiseMonitoringOwningFacade::State& state, std::size_t ordinal) noexcept
    -> std::expected<void, PairwiseMonitoringCaptureFailure> {
    if (!state.active_slot ||
        ordinal >= static_cast<std::size_t>(state.capacity.round_capacity()))
        std::terminate();
    auto& slot = *state.active_slot;
    auto& platform = *state.retirement_platform;
    monitoring::detail::PairwisePreparedCaptureStorageAccess::
        increment_scheduled(slot);
    auto context = read_context_round(state, ordinal,
        ordinal == 0U ? PairwisePerfDrainRole::initial : PairwisePerfDrainRole::scheduled);
    if (!context)
        return context;
    if (state.temperature_supported) {
        if (!state.temperature_source || !state.temperature_workspace)
            std::terminate();
        monitoring::detail::PairwisePreparedCaptureStorageAccess::
            increment_temperature(slot);
        auto started = platform.raw_monotonic_now();
        if (!started)
            return std::unexpected{scheduler_failure(std::move(started.error()))};
        auto value = platform.read_hwmon_temperature(
            *state.temperature_source, *state.temperature_workspace);
        if (!value)
            return std::unexpected{operation_failure(std::move(value.error()))};
        auto completed = platform.raw_monotonic_now();
        if (!completed)
            return std::unexpected{scheduler_failure(std::move(completed.error()))};
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                append_temperature_acquisition(
                    slot, ordinal, *started, *completed, *value))
            std::terminate();
    }
    if (state.frequency_supported) {
        if (!state.frequency_sources ||
            state.frequency_sources->domain_count() !=
                state.frequency_domain_count ||
            state.frequency_workspaces.size() < state.frequency_domain_count)
            std::terminate();
        for (std::size_t domain{};
             domain < state.frequency_domain_count; ++domain) {
            monitoring::detail::PairwisePreparedCaptureStorageAccess::
                increment_frequency(slot);
            auto started = platform.raw_monotonic_now();
            if (!started)
                return std::unexpected{
                    scheduler_failure(std::move(started.error()))};
            auto value = platform.read_cpu_frequency(
                *state.frequency_sources, domain,
                state.frequency_workspaces[domain]);
            if (!value)
                return std::unexpected{
                    operation_failure(std::move(value.error()))};
            auto completed = platform.raw_monotonic_now();
            if (!completed)
                return std::unexpected{
                    scheduler_failure(std::move(completed.error()))};
            const auto& metadata = state.frequency_metadata[domain];
            if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    append_frequency_acquisition(
                        slot, domain, ordinal, metadata.cpus, metadata.kind,
                        *started, *completed, *value))
                std::terminate();
        }
    }
    if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::commit_round(
            slot, ordinal, state.temperature_supported,
            state.frequency_supported))
        std::terminate();
    monitoring::detail::PairwisePreparedCaptureStorageAccess::
        increment_completed(slot);
    return {};
}

void monitoring_service_main(PairwiseMonitoringOwningFacade::State* state,
                             std::stop_token) noexcept {
    auto& async = *state->async;
    const auto publish_startup_failure = [&async](auto value) noexcept {
        std::lock_guard lock{async.mutex};
        async.startup_failure.emplace(std::move(value));
        async.condition.notify_all();
    };
    if (async.missing_placement_operation || !async.service_cpu ||
        !async.affinity_workspace ||
        !state->retirement_platform) {
        publish_startup_failure(monitoring_missing_service_binding_failure());
        return;
    }
    auto placed = state->retirement_platform->set_current_thread_affinity_prepared(
        *async.service_cpu, *async.affinity_workspace);
    if (!placed) {
        auto failure = PairwiseMonitoringOperationFailure::from_service_platform(
            std::move(placed.error()));
        if (failure)
            publish_startup_failure(std::move(*failure));
        else
            publish_startup_failure(local_preparation_failure());
        return;
    }
    auto affinity = state->retirement_platform->
        current_thread_affinity_is_singleton_prepared(
            *async.service_cpu, *async.affinity_workspace);
    if (!affinity) {
        auto failure = PairwiseMonitoringOperationFailure::from_service_platform(
            std::move(affinity.error()));
        if (failure)
            publish_startup_failure(std::move(*failure));
        else
            publish_startup_failure(local_preparation_failure());
        return;
    }
    if (!*affinity) {
        publish_startup_failure(local_preparation_failure());
        return;
    }
    auto current_cpu_result = state->retirement_platform->current_cpu();
    if (!current_cpu_result) {
        auto failure = PairwiseMonitoringOperationFailure::from_service_platform(
            std::move(current_cpu_result.error()));
        if (failure)
            publish_startup_failure(std::move(*failure));
        else
            publish_startup_failure(local_preparation_failure());
        return;
    }
    if (*current_cpu_result != *async.service_cpu) {
        publish_startup_failure(local_preparation_failure());
        return;
    }
    {
        std::lock_guard lock{async.mutex};
        if (!state->preparation_checkpoint.service_placed(
                *async.service_cpu, *async.service_cpu, *current_cpu_result))
            std::terminate();
        async.startup_ready = true;
        async.condition.notify_all();
    }
    try {
        for (;;) {
            std::unique_lock lock{async.mutex};
            async.condition.wait(lock, [&] {
                return async.arm_requested || async.lifetime_stop;
            });
            if (async.lifetime_stop)
                return;
            const auto generation = async.generation;
            const auto run_fault = async.run_fault;
            async.run_fault = PairwiseMonitoringServiceRunFault::none;
            lock.unlock();

            if (run_fault ==
                PairwiseMonitoringServiceRunFault::contained_standard)
                throw std::runtime_error{"scripted Monitoring service failure"};
            if (run_fault ==
                PairwiseMonitoringServiceRunFault::contained_unknown)
                throw 0;

            auto reset = reset_context_sources(*state);
            if (reset)
                async.origin = *reset;
            auto round = [&]()
                -> std::expected<void, PairwiseMonitoringCaptureFailure> {
                if (!reset)
                    return std::unexpected{
                        reset_failure_primary(std::move(reset.error()))};
                return read_point_round(*state, 0U);
            }();

            if (!round) {
                auto failure = std::move(round.error());
                std::optional<PairwiseMonitoringCaptureFailure> finalized;
                if (reset) {
                    auto cleared = reset_context_sources(*state, false);
                    if (!cleared) {
                        if (auto* sequence =
                                std::get_if<PairwiseMonitoringResetFailureSequence>(
                                    &cleared.error()))
                            finalized.emplace(append_reset_cleanup(
                                std::move(failure), std::move(*sequence)));
                        else
                            state->capture_sources_terminal = true;
                    }
                }
                if (!finalized)
                    finalized.emplace(std::move(failure));
                if (!state->context_sources.empty())
                    state->capture_sources_terminal = true;
                lock.lock();
                async.arm_requested = false;
                async.terminal_failure.emplace(std::move(*finalized));
                async.publication_enabled = false;
                async.quiesced = true;
                async.condition.notify_all();
                continue;
            }
            lock.lock();
            async.arm_requested = false;
            async.armed = true;
            async.next_ordinal = 1U;
            async.condition.notify_all();

            for (;;) {
                if (async.lifetime_stop || async.finish_requested ||
                    async.abort_requested || !async.publication_enabled ||
                    async.generation != generation)
                    break;
                auto interval =
                    state->capacity.admission().interval_nanoseconds();
                if (interval && (state->capacity.temperature_requested() ||
                                 state->capacity.frequency_requested()))
                    interval = monitoring::detail::point_acquisition_period_nanoseconds(
                        *interval);
                if (!interval) {
                    async.condition.wait(lock, [&] {
                        return async.lifetime_stop || async.finish_requested ||
                            async.abort_requested ||
                            !async.publication_enabled ||
                            async.generation != generation;
                    });
                    continue;
                }
                const auto ordinal64 =
                    static_cast<std::uint64_t>(async.next_ordinal);
                if (ordinal64 != 0U &&
                    *interval >
                        std::numeric_limits<std::uint64_t>::max() / ordinal64) {
                    async.publication_enabled = false;
                    async.terminal_failure.emplace(monitoring_deadline_failure());
                    break;
                }
                const auto delta = *interval * ordinal64;
                if (delta > std::numeric_limits<std::uint64_t>::max() -
                                async.origin.nanoseconds()) {
                    async.publication_enabled = false;
                    async.terminal_failure.emplace(monitoring_deadline_failure());
                    break;
                }
                const auto deadline =
                    platform_linux::RawMonotonicTimestamp::from_nanoseconds(
                        async.origin.nanoseconds() + delta);
                lock.unlock();
                auto waited = monitoring::detail::wait_until_raw_monotonic(
                    async.scheduler_mutex, async.scheduler_condition,
                    async.scheduler_wake, deadline, [&]() noexcept {
                        return state->retirement_platform->raw_monotonic_now();
                    }, [](std::condition_variable& condition,
                          std::unique_lock<std::mutex>& scheduler_lock,
                          std::uint64_t nanoseconds) noexcept {
                        using Rep = std::chrono::nanoseconds::rep;
                        condition.wait_for(
                            scheduler_lock,
                            std::chrono::nanoseconds{static_cast<Rep>(nanoseconds)});
                    });
                lock.lock();
                if (!waited) {
                    async.publication_enabled = false;
                    async.terminal_failure.emplace(
                        scheduler_failure(std::move(waited.error())));
                    break;
                }
                if (*waited ==
                    monitoring::detail::InterruptibleRawWaitResult::interrupted)
                    continue;
                monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    increment_wakeup(*state->active_slot);
                if (async.next_ordinal >=
                    static_cast<std::size_t>(state->capacity.round_capacity())) {
                    async.publication_enabled = false;
                    async.terminal_failure.emplace(
                        monitoring_schedule_capacity_failure());
                    break;
                }
                auto current = state->retirement_platform->raw_monotonic_now();
                if (!current) {
                    async.publication_enabled = false;
                    async.terminal_failure.emplace(
                        scheduler_failure(std::move(current.error())));
                    break;
                }
                bool skip_terminal{};
                for (;;) {
                    if (async.next_ordinal >= static_cast<std::size_t>(
                                                  state->capacity.round_capacity())) {
                        async.publication_enabled = false;
                        async.terminal_failure.emplace(
                            monitoring_schedule_capacity_failure());
                        skip_terminal = true;
                        break;
                    }
                    const auto current_ordinal =
                        static_cast<std::uint64_t>(async.next_ordinal);
                    if (current_ordinal ==
                        std::numeric_limits<std::uint64_t>::max()) {
                        async.publication_enabled = false;
                        async.terminal_failure.emplace(
                            monitoring_deadline_failure());
                        skip_terminal = true;
                        break;
                    }
                    const auto following_ordinal = current_ordinal + 1U;
                    if (*interval >
                        std::numeric_limits<std::uint64_t>::max() /
                            following_ordinal) {
                        async.publication_enabled = false;
                        async.terminal_failure.emplace(
                            monitoring_deadline_failure());
                        skip_terminal = true;
                        break;
                    }
                    const auto following_delta =
                        *interval * following_ordinal;
                    if (following_delta >
                        std::numeric_limits<std::uint64_t>::max() -
                            async.origin.nanoseconds()) {
                        async.publication_enabled = false;
                        async.terminal_failure.emplace(
                            monitoring_deadline_failure());
                        skip_terminal = true;
                        break;
                    }
                    const auto following_deadline =
                        platform_linux::RawMonotonicTimestamp::from_nanoseconds(
                            async.origin.nanoseconds() + following_delta);
                    if (*current < following_deadline)
                        break;
                    monitoring::detail::PairwisePreparedCaptureStorageAccess::
                        increment_missed(*state->active_slot);
                    ++async.next_ordinal;
                }
                if (skip_terminal)
                    break;
                const auto ordinal = async.next_ordinal++;
                async.round_in_flight = true;
                lock.unlock();
                auto acquired = read_point_round(*state, ordinal);
                lock.lock();
                async.round_in_flight = false;
                if (!acquired) {
                    async.publication_enabled = false;
                    async.terminal_failure.emplace(
                        std::move(acquired.error()));
                    break;
                }
            }

            const auto retain_final = async.finish_requested &&
                !async.abort_requested && !async.lifetime_stop &&
                !async.terminal_failure && async.generation == generation;
            std::optional<PairwiseMonitoringCaptureFailure> generation_failure;
            if (async.terminal_failure) {
                generation_failure.emplace(
                    std::move(*async.terminal_failure));
                async.terminal_failure.reset();
            }
            lock.unlock();
            async.reset_failures.reset();
            if (retain_final) {
                auto closed = close_context_epoch(*state);
                if (!closed) generation_failure.emplace(std::move(closed.error()));
                else {
                    auto final = read_context_round(*state,
                        static_cast<std::size_t>(state->capacity.round_capacity()),
                        PairwisePerfDrainRole::final);
                    if (!final) generation_failure.emplace(std::move(final.error()));
                    else {
                        auto verified = verify_context_epoch_loss(*state);
                        if (!verified) generation_failure.emplace(std::move(verified.error()));
                    }
                }
            }
            if (!retain_final) {
                auto cleared = reset_context_sources(*state, false);
                if (!cleared) {
                    if (auto* sequence =
                            std::get_if<PairwiseMonitoringResetFailureSequence>(
                                &cleared.error())) {
                        if (generation_failure) {
                            auto combined = append_reset_cleanup(
                                std::move(*generation_failure),
                                std::move(*sequence));
                            generation_failure.reset();
                            generation_failure.emplace(std::move(combined));
                        } else {
                            async.reset_failures.emplace(std::move(*sequence));
                        }
                    } else if (!generation_failure) {
                        generation_failure.emplace(std::move(std::get<
                            PairwiseMonitoringCaptureFailure>(
                            cleared.error())));
                    }
                }
            }
            if (!state->context_sources.empty() && (generation_failure || !retain_final))
                state->capture_sources_terminal = true;
            lock.lock();
            if (generation_failure)
                async.terminal_failure.emplace(
                    std::move(*generation_failure));
            async.armed = false;
            async.finish_requested = false;
            async.abort_requested = false;
            async.publication_enabled = false;
            async.quiesced = true;
            async.round_in_flight = false;
            async.condition.notify_all();
        }
    } catch (const std::exception&) {
        auto failure = monitoring_service_contained_failure(false);
        std::optional<PairwiseMonitoringCaptureFailure> finalized;
        auto cleared = reset_context_sources(*state, false);
        if (!cleared) {
            if (auto* sequence =
                    std::get_if<PairwiseMonitoringResetFailureSequence>(
                        &cleared.error()))
                finalized.emplace(append_reset_cleanup(
                    std::move(failure), std::move(*sequence)));
            state->capture_sources_terminal = true;
        }
        state->service_terminal = true;
        std::lock_guard lock{async.mutex};
        if (!async.terminal_failure)
            async.terminal_failure.emplace(finalized
                ? std::move(*finalized)
                : std::move(failure));
        async.publication_enabled = false;
        async.quiesced = true;
        async.condition.notify_all();
    } catch (...) {
        auto failure = monitoring_service_contained_failure(true);
        std::optional<PairwiseMonitoringCaptureFailure> finalized;
        auto cleared = reset_context_sources(*state, false);
        if (!cleared) {
            if (auto* sequence =
                    std::get_if<PairwiseMonitoringResetFailureSequence>(
                        &cleared.error()))
                finalized.emplace(append_reset_cleanup(
                    std::move(failure), std::move(*sequence)));
            state->capture_sources_terminal = true;
        }
        state->service_terminal = true;
        std::lock_guard lock{async.mutex};
        if (!async.terminal_failure)
            async.terminal_failure.emplace(finalized
                ? std::move(*finalized)
                : std::move(failure));
        async.publication_enabled = false;
        async.quiesced = true;
        async.condition.notify_all();
    }
}

} // namespace

auto PairwiseMonitoringOperationFailure::from_monitoring(
    PairwiseMonitoringFailure failure) noexcept -> PairwiseMonitoringOperationFailure {
    return PairwiseMonitoringOperationFailure{Value{std::move(failure)}};
}

auto PairwiseMonitoringOperationFailure::from_platform(
    PairwisePlatformFailure failure) noexcept
    -> std::optional<PairwiseMonitoringOperationFailure> {
    if (!monitoring_platform_code(failure.code()))
        return std::nullopt;
    return PairwiseMonitoringOperationFailure{Value{std::move(failure)}};
}

auto PairwiseMonitoringOperationFailure::from_service_platform(
    PairwisePlatformFailure failure) noexcept
    -> std::optional<PairwiseMonitoringOperationFailure> {
    if (!service_placement_platform_relation(failure.code(),
                                             failure.operation()))
        return std::nullopt;
    return PairwiseMonitoringOperationFailure{Value{std::move(failure)}};
}

auto PairwiseMonitoringOperationFailure::subsystem() const noexcept
    -> PairwiseFailureSubsystem {
    return std::visit([](const auto& value) { return value.subsystem(); }, value_);
}
auto PairwiseMonitoringOperationFailure::code() const noexcept -> PairwiseFailureCode {
    return std::visit([](const auto& value) { return value.code(); }, value_);
}
auto PairwiseMonitoringOperationFailure::operation() const noexcept
    -> PairwiseFailureOperation {
    return std::visit([](const auto& value) { return value.operation(); }, value_);
}
auto PairwiseMonitoringOperationFailure::reason() const noexcept -> PairwiseFailureReason {
    return std::visit([](const auto& value) { return value.reason(); }, value_);
}
auto PairwiseMonitoringOperationFailure::context() const noexcept
    -> const PairwiseFailureContext& {
    return std::visit([](const auto& value) -> const PairwiseFailureContext& {
        return value.context();
    }, value_);
}
auto PairwiseMonitoringOperationFailure::posix_errno() const noexcept
    -> std::optional<int> {
    return std::visit([](const auto& value) { return value.posix_errno(); }, value_);
}

auto PairwiseMonitoringCaptureFailureCursor::consume(
    PairwiseMonitoringCaptureFailure&& source) noexcept
    -> PairwiseMonitoringCaptureFailureCursor {
    return PairwiseMonitoringCaptureFailureCursor{std::move(source)};
}

PairwiseMonitoringCaptureFailureCursor::
PairwiseMonitoringCaptureFailureCursor(
    PairwiseMonitoringCaptureFailureCursor&& other) noexcept
    : source_{std::move(other.source_)},
      cleanup_index_{other.cleanup_index_},
      primary_pending_{other.primary_pending_},
      active_{std::exchange(other.active_, false)} {
    if (!active_)
        std::terminate();
}

void PairwiseMonitoringCaptureFailureCursor::require_active() const noexcept {
    if (!active_)
        std::terminate();
}

auto PairwiseMonitoringCaptureFailureCursor::empty() const noexcept -> bool {
    require_active();
    return !primary_pending_ && cleanup_index_ == source_.cleanup_count_;
}

auto PairwiseMonitoringCaptureFailureCursor::project(
    PairwiseMonitoringOperationFailure& source) noexcept
    -> PairwiseEvidenceFailure {
    return std::visit([](auto& value) -> PairwiseEvidenceFailure {
        return std::move(value);
    }, source.value_);
}

auto PairwiseMonitoringCaptureFailureCursor::next() noexcept
    -> PairwiseEvidenceFailure {
    require_active();
    if (primary_pending_) {
        primary_pending_ = false;
        return std::visit([](auto& value) -> PairwiseEvidenceFailure {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value,
                          PairwiseMonitoringOperationFailure>)
                return PairwiseMonitoringCaptureFailureCursor::project(value);
            else
                return PairwiseEvidenceFailure{std::move(value.value_)};
        }, source_.primary_);
    }
    if (cleanup_index_ >= source_.cleanup_count_)
        std::terminate();
    const auto position = source_.cleanup_offset_ + cleanup_index_++;
    if (position >= source_.cleanup_.size() || !source_.cleanup_[position])
        std::terminate();
    return project(*source_.cleanup_[position]);
}

auto PairwiseMonitoringSchedulerFailure::from_platform(
    PairwisePlatformFailure failure) noexcept
    -> std::optional<PairwiseMonitoringSchedulerFailure> {
    switch (failure.code()) {
    case PairwiseFailureCode::platform_raw_clock_read_failed:
    case PairwiseFailureCode::platform_raw_clock_conversion_failed:
    case PairwiseFailureCode::platform_raw_clock_invalid_timestamp:
    case PairwiseFailureCode::platform_raw_clock_contained_exception:
    case PairwiseFailureCode::platform_raw_clock_contained_unknown_failure:
        if (failure.operation() ==
            PairwiseFailureOperation::raw_monotonic_now)
            return PairwiseMonitoringSchedulerFailure{std::move(failure)};
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

PairwiseMonitoringPreparationFailure::PairwiseMonitoringPreparationFailure(
    PairwiseMonitoringPreparationFailure&& other) noexcept
    : value_{std::move(other.value_)}, cleanup_{std::move(other.cleanup_)},
      source_retirement_{std::move(other.source_retirement_)},
      source_retirement_count_{other.source_retirement_count_},
      frequency_source_retirement_count_{
          other.frequency_source_retirement_count_},
      ordinary_service_join_{other.ordinary_service_join_},
      force_dispositions_{other.force_dispositions_},
      force_disposition_count_{other.force_disposition_count_},
      active_{std::exchange(other.active_, false)} {
    if (!active_)
        std::terminate();
}
void PairwiseMonitoringPreparationFailure::require_active() const noexcept {
    if (!active_)
        std::terminate();
}

auto PairwiseMonitoringPreparationFailure::runtime(
    PairwiseRuntimeFailure failure) noexcept -> PairwiseMonitoringPreparationFailure {
    return PairwiseMonitoringPreparationFailure{Value{std::move(failure)}};
}
auto PairwiseMonitoringPreparationFailure::with_source_retirement(
    PairwiseMonitoringPreparationFailure failure,
    std::vector<platform_linux::detail::BoundedSysfsDisposition>
        source_retirement,
    std::size_t source_retirement_count,
    std::size_t frequency_source_retirement_count) noexcept
    -> PairwiseMonitoringPreparationFailure {
    failure.require_active();
    if (frequency_source_retirement_count > source_retirement_count)
        std::terminate();
    failure.source_retirement_ = std::move(source_retirement);
    failure.source_retirement_count_ = source_retirement_count;
    failure.frequency_source_retirement_count_ =
        frequency_source_retirement_count;
    return failure;
}
auto PairwiseMonitoringPreparationFailure::with_service_terminalization(
    PairwiseMonitoringPreparationFailure failure,
    PairwiseMonitoringServiceJoinFault ordinary,
    std::array<PairwiseMonitoringForceDisposition, 3> force_dispositions,
    std::size_t force_disposition_count) noexcept
    -> PairwiseMonitoringPreparationFailure {
    failure.require_active();
    failure.ordinary_service_join_ = ordinary;
    failure.force_dispositions_ = force_dispositions;
    failure.force_disposition_count_ = force_disposition_count;
    return failure;
}
auto PairwiseMonitoringPreparationFailure::operation(
    PairwiseMonitoringOperationFailure failure,
    std::vector<PairwiseMonitoringOperationFailure> cleanup,
    std::vector<platform_linux::detail::BoundedSysfsDisposition>
        source_retirement,
    std::size_t source_retirement_count,
    std::size_t frequency_source_retirement_count) noexcept
    -> PairwiseMonitoringPreparationFailure {
    if (frequency_source_retirement_count > source_retirement_count)
        std::terminate();
    return PairwiseMonitoringPreparationFailure{
        Value{std::move(failure)}, std::move(cleanup),
        std::move(source_retirement), source_retirement_count,
        frequency_source_retirement_count};
}
auto PairwiseMonitoringPreparationFailure::subsystem() const noexcept
    -> PairwiseFailureSubsystem {
    require_active();
    return std::visit([](const auto& value) { return value.subsystem(); }, value_);
}
auto PairwiseMonitoringPreparationFailure::code() const noexcept -> PairwiseFailureCode {
    require_active();
    return std::visit([](const auto& value) { return value.code(); }, value_);
}
auto PairwiseMonitoringPreparationFailure::operation_name() const noexcept
    -> PairwiseFailureOperation {
    require_active();
    return std::visit([](const auto& value) { return value.operation(); }, value_);
}
auto PairwiseMonitoringPreparationFailure::reason() const noexcept
    -> PairwiseFailureReason {
    require_active();
    return std::visit([](const auto& value) { return value.reason(); }, value_);
}
auto PairwiseMonitoringPreparationFailure::posix_errno() const noexcept
    -> std::optional<int> {
    require_active();
    return std::visit([](const auto& value) { return value.posix_errno(); }, value_);
}

PairwiseMonitoringStopOutcome::PairwiseMonitoringStopOutcome(
    PairwiseMonitoringStopOutcome&& other) noexcept
    : failures_{std::move(other.failures_)},
      failure_count_{other.failure_count_},
      source_retirement_{std::move(other.source_retirement_)},
      source_retirement_count_{other.source_retirement_count_},
      frequency_source_retirement_count_{
          other.frequency_source_retirement_count_},
      force_dispositions_{other.force_dispositions_},
      force_disposition_count_{other.force_disposition_count_},
      invariant_failed_{other.invariant_failed_},
      active_{std::exchange(other.active_, false)} {
    if (!active_)
        std::terminate();
}
void PairwiseMonitoringStopOutcome::require_active() const noexcept {
    if (!active_)
        std::terminate();
}

PairwiseMonitoringFailureCursor::PairwiseMonitoringFailureCursor(
    PairwiseMonitoringPreparationFailure&& source) noexcept
    : kind_{Kind::preparation}, source_{std::in_place_index<0>,
                                      std::move(source)},
      primary_pending_{true}, ordinary_pending_{true} {}
PairwiseMonitoringFailureCursor::PairwiseMonitoringFailureCursor(
    PairwiseMonitoringStopOutcome&& source) noexcept
    : kind_{Kind::stop}, source_{std::in_place_index<1>, std::move(source)},
      invariant_pending_{std::get<1>(source_).invariant_failed_} {}
PairwiseMonitoringFailureCursor::PairwiseMonitoringFailureCursor(
    PairwiseMonitoringFailureCursor&& other) noexcept
    : kind_{other.kind_}, source_{std::move(other.source_)},
      operation_index_{other.operation_index_}, source_index_{other.source_index_},
      primary_pending_{other.primary_pending_},
      ordinary_pending_{other.ordinary_pending_},
      invariant_pending_{other.invariant_pending_},
      active_{std::exchange(other.active_, false)} {
    if (!active_)
        std::terminate();
}
void PairwiseMonitoringFailureCursor::require_active() const noexcept {
    if (!active_)
        std::terminate();
}
auto PairwiseMonitoringFailureCursor::consume(
    PairwiseMonitoringPreparationFailure&& source) noexcept
    -> PairwiseMonitoringFailureCursor {
    source.require_active();
    return PairwiseMonitoringFailureCursor{std::move(source)};
}
auto PairwiseMonitoringFailureCursor::consume(
    PairwiseMonitoringStopOutcome&& source) noexcept
    -> PairwiseMonitoringFailureCursor {
    source.require_active();
    return PairwiseMonitoringFailureCursor{std::move(source)};
}

namespace {
auto ordinary_service_join_failure(
    PairwiseMonitoringServiceJoinFault fault) noexcept
    -> PairwiseMonitoringOperationFailure {
    if (fault == PairwiseMonitoringServiceJoinFault::success)
        std::terminate();
    return monitoring_stop_contained(
        fault == PairwiseMonitoringServiceJoinFault::contained_unknown
            ? PairwiseFailureReason::contained_unknown_failure
            : PairwiseFailureReason::contained_exception);
}
bool failed_disposition(
    const platform_linux::detail::BoundedSysfsDisposition& value) noexcept {
    return value.kind !=
        platform_linux::detail::BoundedSysfsDispositionKind::success;
}
} // namespace

auto PairwiseMonitoringFailureCursor::project_operation(
    PairwiseMonitoringOperationFailure&& source) noexcept
    -> PairwiseEvidenceFailure {
    return std::visit([](auto& value) -> PairwiseEvidenceFailure {
        return std::move(value);
    }, source.value_);
}

auto PairwiseMonitoringFailureCursor::preparation_empty() const noexcept -> bool {
    const auto& source = std::get<0>(source_);
    if (primary_pending_)
        return false;
    if (ordinary_pending_ && source.ordinary_service_join_ &&
        *source.ordinary_service_join_ != PairwiseMonitoringServiceJoinFault::success)
        return false;
    if (operation_index_ < source.cleanup_.size())
        return false;
    for (auto index = source_index_; index < source.source_retirement_count_; ++index)
        if (failed_disposition(source.source_retirement_[index]))
            return false;
    return !invariant_pending_;
}
auto PairwiseMonitoringFailureCursor::stop_empty() const noexcept -> bool {
    const auto& source = std::get<1>(source_);
    if (operation_index_ < source.failure_count_)
        return false;
    for (auto index = source_index_; index < source.source_retirement_count_; ++index)
        if (failed_disposition(source.source_retirement_[index]))
            return false;
    return !invariant_pending_;
}
auto PairwiseMonitoringFailureCursor::empty() const noexcept -> bool {
    require_active();
    return kind_ == Kind::preparation ? preparation_empty() : stop_empty();
}
auto PairwiseMonitoringFailureCursor::next_preparation() noexcept
    -> PairwiseEvidenceFailure {
    auto& source = std::get<0>(source_);
    if (primary_pending_) {
        primary_pending_ = false;
        return std::visit([](auto& value) -> PairwiseEvidenceFailure {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Value, PairwiseRuntimeFailure>)
                return std::move(value);
            else
                return project_operation(std::move(value));
        }, source.value_);
    }
    if (ordinary_pending_) {
        ordinary_pending_ = false;
        if (source.ordinary_service_join_ &&
            *source.ordinary_service_join_ != PairwiseMonitoringServiceJoinFault::success)
            return project_operation(ordinary_service_join_failure(
                *source.ordinary_service_join_));
    }
    if (operation_index_ < source.cleanup_.size())
        return project_operation(std::move(source.cleanup_[operation_index_++]));
    while (source_index_ < source.source_retirement_count_) {
        const auto index = source_index_++;
        const auto value = source.source_retirement_[index];
        if (failed_disposition(value))
            return project_operation(source_retirement_failure(
                index < source.frequency_source_retirement_count_, value));
    }
    if (invariant_pending_) {
        invariant_pending_ = false;
        return monitoring_projection_invariant();
    }
    std::terminate();
}
auto PairwiseMonitoringFailureCursor::next_stop() noexcept
    -> PairwiseEvidenceFailure {
    auto& source = std::get<1>(source_);
    while (operation_index_ < source.failure_count_) {
        const auto index = operation_index_++;
        if (source.failures_[index])
            return project_operation(std::move(*source.failures_[index]));
    }
    while (source_index_ < source.source_retirement_count_) {
        const auto index = source_index_++;
        const auto value = source.source_retirement_[index];
        if (failed_disposition(value))
            return project_operation(source_retirement_failure(
                index < source.frequency_source_retirement_count_, value));
    }
    if (invariant_pending_) {
        invariant_pending_ = false;
        return monitoring_projection_invariant();
    }
    std::terminate();
}
auto PairwiseMonitoringFailureCursor::next() noexcept
    -> PairwiseEvidenceFailure {
    require_active();
    if (empty())
        std::terminate();
    return kind_ == Kind::preparation ? next_preparation() : next_stop();
}

PairwiseMonitoringOwningFacade::PairwiseMonitoringOwningFacade(
    std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
PairwiseMonitoringOwningFacade::PairwiseMonitoringOwningFacade(
    PairwiseMonitoringOwningFacade&&) noexcept = default;
PairwiseMonitoringOwningFacade::~PairwiseMonitoringOwningFacade() noexcept {
    if (state_ && state_->retirement_platform)
        static_cast<void>(stop_and_retire(*state_->retirement_platform));
}

PairwisePreparedMonitoringStorage::PairwisePreparedMonitoringStorage(
    std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
PairwisePreparedMonitoringStorage::PairwisePreparedMonitoringStorage(
    PairwisePreparedMonitoringStorage&&) noexcept = default;
auto PairwisePreparedMonitoringStorage::operator=(
    PairwisePreparedMonitoringStorage&&) noexcept
    -> PairwisePreparedMonitoringStorage& = default;
PairwisePreparedMonitoringStorage::~PairwisePreparedMonitoringStorage() noexcept = default;

PairwiseBoundMonitoringPreparation::PairwiseBoundMonitoringPreparation(
    std::unique_ptr<PairwisePreparedMonitoringStorage::State> state) noexcept
    : state_{std::move(state)} {}
PairwiseBoundMonitoringPreparation::PairwiseBoundMonitoringPreparation(
    PairwiseBoundMonitoringPreparation&&) noexcept = default;
auto PairwiseBoundMonitoringPreparation::operator=(
    PairwiseBoundMonitoringPreparation&&) noexcept
    -> PairwiseBoundMonitoringPreparation& = default;
PairwiseBoundMonitoringPreparation::~PairwiseBoundMonitoringPreparation() noexcept = default;

auto PairwiseMonitoringOwningFacade::prepare_capacity_storage(
    PairwiseAdmittedMonitoringCapacity capacity,
    const PairwisePlatformLeafFacade& platform,
    BoundedMonitoringStorageDependencies dependencies) noexcept
    -> std::expected<PairwisePreparedMonitoringStorage,
                     PairwiseMonitoringPreparationFailure> {
    try {
        const auto workers = checked_size(capacity.workers());
        const auto domains = checked_size(capacity.maximum_domain_count());
        const auto memberships = checked_size(capacity.maximum_membership_count());
        const auto rollback = checked_size(capacity.preparation_rollback_values());
        if (!workers || !domains || !memberships || !rollback || *rollback == 0U ||
            dependencies.worker_cpus.size() != *workers ||
            !std::ranges::equal(dependencies.worker_cpus,
                                capacity.admission().worker_cpus()))
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};

        if (capacity.frequency_requested() &&
            (*domains == 0U || *domains > *memberships))
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        auto allocated = prepare_storage_impl(
            std::move(capacity), {},
            false,
            platform_linux::detail::BoundedCpuFrequencyValidatedDisposition::unsupported,
            {}, false, true);
        if (!allocated)
            return std::unexpected{std::move(allocated.error())};

        auto prepared = std::make_unique<PairwisePreparedMonitoringStorage::State>();
        prepared->storage = std::move(allocated->state_);
        // Stage 1 provisions requested channel storage.  Support is a Stage 2
        // fact, so these flags are reset until the validation gates commit.
        prepared->storage->temperature_supported = false;
        prepared->storage->frequency_supported = false;
        prepared->storage->temperature_unavailable = false;
        prepared->storage->frequency_unavailable = false;
        const auto rollback_cleanup = monitoring_rollback_cleanup_capacity(
            prepared->storage->capacity.temperature_requested(),
            prepared->storage->capacity.frequency_requested(), *domains,
            *rollback);
        if (!rollback_cleanup)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        prepared->rollback_cleanup.reserve(*rollback_cleanup);

        if (prepared->storage->capacity.asynchronous_requested()) {
            if (!prepared->storage->capacity.service_cpu())
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::operation(
                        monitoring_missing_service_binding_failure())};
            const auto affinity_capacity = checked_size(
                prepared->storage->capacity.kernel_cpu_count());
            if (!affinity_capacity)
                return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                    local_preparation_failure())};
            auto workspace = platform.prepare_affinity_workspace(*affinity_capacity);
            if (!workspace) {
                auto mapped = PairwiseMonitoringOperationFailure::from_service_platform(
                    std::move(workspace.error()));
                if (!mapped)
                    return std::unexpected{
                        PairwiseMonitoringPreparationFailure::runtime(
                            local_preparation_failure())};
                return std::unexpected{PairwiseMonitoringPreparationFailure::operation(
                    std::move(*mapped))};
            }
            prepared->service_affinity_workspace.emplace(std::move(*workspace));
        }
        if (prepared->storage->capacity.context_requested()) {
            prepared->context_workspaces.reserve(*workers);
            for (std::size_t index{}; index < *workers; ++index) {
                auto workspace = platform.prepare_perf_context_switch_workspace();
                if (!workspace) {
                    auto mapped = PairwiseMonitoringOperationFailure::from_platform(
                        std::move(workspace.error()));
                    if (!mapped)
                        return std::unexpected{
                            PairwiseMonitoringPreparationFailure::runtime(
                                local_preparation_failure())};
                    return std::unexpected{
                        PairwiseMonitoringPreparationFailure::operation(
                            std::move(*mapped))};
                }
                prepared->context_workspaces.push_back(std::move(*workspace));
            }
        }
        const auto retirement_capacity = checked_add_size(2U * *domains + 1U, 3U);
        if (!retirement_capacity)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        prepared->storage->source_retirement.resize(*retirement_capacity);

        if (prepared->storage->capacity.temperature_requested()) {
            auto value = platform.prepare_hwmon_discovery_storage(
                dependencies.temperature_capacity,
                dependencies.temperature_operations);
            if (!value)
                return std::unexpected{monitoring_preparation_failure(
                    std::move(value.error()))};
            prepared->temperature_discovery.emplace(std::move(*value));
        }
        if (prepared->storage->capacity.frequency_requested()) {
            auto value = platform.prepare_cpu_frequency_discovery_storage(
                dependencies.frequency_capacity, dependencies.present_cpus,
                dependencies.worker_cpus, dependencies.frequency_operations);
            if (!value)
                return std::unexpected{monitoring_preparation_failure(
                    std::move(value.error()))};
            prepared->frequency_discovery.emplace(std::move(*value));
        }
        return PairwisePreparedMonitoringStorage{std::move(prepared)};
    } catch (...) {
        return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure())};
    }
}

auto PairwisePreparedMonitoringStorage::bind_worker_readiness(
    PairwiseWorkerReadinessCapability capability) && noexcept
    -> std::expected<PairwiseBoundMonitoringPreparation,
                     PairwiseMonitoringPreparationFailure> {
    auto prepared = std::move(state_);
    if (!prepared || prepared->consumed || !prepared->storage)
        return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure())};
    prepared->consumed = true;
    auto [specs, readiness] = capability.consume();
    const auto expected = static_cast<std::size_t>(prepared->storage->capacity.workers());
    if (readiness.size() != expected || specs.size() != expected)
        return std::unexpected{worker_count_failure(expected, readiness.size())};
    const auto admitted = prepared->storage->capacity.admission().worker_cpus();
    for (std::size_t index{}; index < expected; ++index) {
        const auto& spec = specs[index];
        const auto& ready = readiness[index];
        const auto expected_id = core::WorkerId{static_cast<std::uint32_t>(index)};
        bool mismatch = spec.worker_index != index || ready.worker_index != index ||
            spec.worker_id != expected_id || ready.worker_id != expected_id ||
            ready.worker_id != spec.worker_id ||
            ready.stable_pu != spec.stable_pu ||
            ready.stable_core != spec.stable_core ||
            spec.requested_cpu != admitted[index] ||
            ready.requested_cpu != admitted[index] ||
            ready.requested_cpu != spec.requested_cpu ||
            ready.observed_cpu != admitted[index] || ready.tid.value() == 0U ||
            ready.observed_affinity.cardinality() != 1U ||
            !ready.observed_affinity.contains(admitted[index]);
        for (std::size_t prior{}; prior < index && !mismatch; ++prior)
            mismatch = readiness[prior].worker_id == ready.worker_id ||
                readiness[prior].requested_cpu == ready.requested_cpu ||
                readiness[prior].tid == ready.tid;
        if (mismatch)
            return std::unexpected{worker_association_failure(index)};
    }
    auto& scopes = prepared->storage->workers;
    if (scopes.capacity() < expected)
        return std::unexpected{
            monitoring_storage_capacity_failure(scopes.capacity(), expected)};
    for (std::size_t index{}; index < expected; ++index)
        scopes.push_back(monitoring::MonitoringWorkerScope::create(
            readiness[index].worker_id, readiness[index].tid,
            readiness[index].requested_cpu));
    prepared->bound = true;
    prepared->consumed = false;
    return PairwiseBoundMonitoringPreparation{std::move(prepared)};
}

auto PairwiseBoundMonitoringPreparation::acquire_sources_and_owner(
    const PairwisePlatformLeafFacade& platform) && noexcept
    -> std::expected<PairwiseMonitoringOwningFacade,
                     PairwiseMonitoringPreparationFailure> {
    auto prepared = std::move(state_);
    if (!prepared || prepared->consumed || !prepared->bound ||
        !prepared->storage)
        return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure())};
    prepared->consumed = true;
    auto& state = *prepared->storage;
    std::optional<PairwisePreparedTemperatureSource> temperature_source;
    std::optional<PairwisePreparedFrequencySources> frequency_sources;
    std::optional<PairwiseMonitoringPreparationFailure> gate_failure;
    std::size_t source_retirement_count{};
    std::size_t frequency_source_retirement_count{};

    const auto append_platform_failure = [&](PairwisePlatformFailure failure) noexcept {
        auto mapped = PairwiseMonitoringOperationFailure::from_platform(
            std::move(failure));
        if (!mapped || prepared->rollback_cleanup.size() ==
                           prepared->rollback_cleanup.capacity())
            return false;
        prepared->rollback_cleanup.push_back(std::move(*mapped));
        return true;
    };
    const auto remaining_retirement = [&]() noexcept
        -> std::span<platform_linux::detail::BoundedSysfsDisposition> {
        if (source_retirement_count >= state.source_retirement.size())
            return {};
        return std::span{state.source_retirement}.subspan(
            source_retirement_count);
    };
    const auto rollback = [&]() noexcept {
        bool invariant{};
        for (auto prior = state.context_sources.rbegin();
             prior != state.context_sources.rend(); ++prior) {
            auto retired = platform.retire_perf_context_switch(std::move(*prior));
            if (retired.primary &&
                !append_platform_failure(std::move(*retired.primary)))
                invariant = true;
            if (retired.cleanup &&
                !append_platform_failure(std::move(*retired.cleanup)))
                invariant = true;
        }
        state.context_sources.clear();
        if (frequency_sources) {
            const auto available = remaining_retirement().size();
            const auto retired = platform.retire_cpu_frequencies(
                std::move(*frequency_sources), remaining_retirement());
            frequency_sources.reset();
            if (retired > available) {
                invariant = true;
                source_retirement_count = state.source_retirement.size();
            } else {
                source_retirement_count += retired;
            }
            frequency_source_retirement_count = source_retirement_count;
        }
        if (temperature_source) {
            const auto available = remaining_retirement().size();
            const auto retired = platform.retire_hwmon_temperature(
                std::move(*temperature_source), remaining_retirement());
            temperature_source.reset();
            if (retired > available) {
                invariant = true;
                source_retirement_count = state.source_retirement.size();
            } else {
                source_retirement_count += retired;
            }
        }
        return invariant;
    };
    const auto fail_invariant = [&]() noexcept {
        static_cast<void>(rollback());
        auto failure = PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure());
        if (source_retirement_count != 0U)
            return PairwiseMonitoringPreparationFailure::with_source_retirement(
                std::move(failure), std::move(state.source_retirement),
                source_retirement_count, frequency_source_retirement_count);
        return failure;
    };
    const auto fail_after_rollback = [&](PairwiseMonitoringPreparationFailure failure)
        noexcept {
        if (rollback())
            return fail_invariant();
        if (source_retirement_count != 0U)
            return PairwiseMonitoringPreparationFailure::with_source_retirement(
                std::move(failure), std::move(state.source_retirement),
                source_retirement_count, frequency_source_retirement_count);
        return failure;
    };
    const auto map_sysfs_failure = [&](PairwiseSysfsPreparationFailure failure)
        noexcept -> PairwiseMonitoringPreparationFailure {
        auto primary = PairwiseMonitoringOperationFailure::from_platform(
            std::move(failure.primary));
        if (!primary)
            return PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure());
        for (auto& cleanup : failure.cleanup)
            if (!append_platform_failure(std::move(cleanup)))
                return PairwiseMonitoringPreparationFailure::runtime(
                    local_preparation_failure());
        return PairwiseMonitoringPreparationFailure::operation(
            std::move(*primary), std::move(prepared->rollback_cleanup));
    };
    struct GateContext final {
        PairwisePreparedMonitoringStorage::State* prepared{};
        PairwiseMonitoringOwningFacade::State* storage{};
        const PairwisePlatformLeafFacade* platform{};
        std::optional<PairwisePreparedTemperatureSource>* temperature_source{};
        std::optional<PairwiseMonitoringPreparationFailure>* failure{};
        bool temperature_supported{};
        bool frequency_supported{};
    } gate_context{prepared.get(), &state, &platform, &temperature_source,
                   &gate_failure, false, false};

    const auto temperature_gate = +[](
        void* raw,
        platform_linux::detail::BoundedHwmonValidatedOutcome&& outcome) noexcept {
        auto& context = *static_cast<GateContext*>(raw);
        context.temperature_supported = outcome.disposition() ==
            platform_linux::detail::BoundedHwmonValidatedDisposition::supported;
        const auto& policy =
            context.storage->capacity.admission().monitoring_policy();
        const auto authored = policy.disposition(
            monitoring::MonitoringChannel::temperature);
        if (!context.temperature_supported &&
            (authored == monitoring::MonitoringChannelDisposition::required ||
             (authored == monitoring::MonitoringChannelDisposition::optional &&
              policy.optional_unsupported() ==
                  monitoring::OptionalUnavailableSourcePolicy::fail))) {
            context.failure->emplace(PairwiseMonitoringPreparationFailure::operation(
                monitoring_required_source_unsupported(
                    monitoring::MonitoringChannel::temperature)));
            return false;
        }
        return true;
    };
    const auto frequency_gate = +[](
        void* raw,
        platform_linux::detail::BoundedCpuFrequencyValidatedGraphOutcome&& outcome)
        noexcept {
        auto& context = *static_cast<GateContext*>(raw);
        auto& storage = *context.storage;
        context.frequency_supported = outcome.disposition() ==
            platform_linux::detail::BoundedCpuFrequencyValidatedDisposition::supported;
        const auto& policy = storage.capacity.admission().monitoring_policy();
        const auto authored = policy.disposition(
            monitoring::MonitoringChannel::frequency);
        if (!context.frequency_supported &&
            (authored == monitoring::MonitoringChannelDisposition::required ||
             (authored == monitoring::MonitoringChannelDisposition::optional &&
              policy.optional_unsupported() ==
                  monitoring::OptionalUnavailableSourcePolicy::fail))) {
            context.failure->emplace(PairwiseMonitoringPreparationFailure::operation(
                monitoring_required_source_unsupported(
                    monitoring::MonitoringChannel::frequency)));
            return false;
        }
        const auto memberships = outcome.domain_memberships();
        if (memberships.size() > storage.frequency_metadata.size()) {
            context.failure->emplace(monitoring_storage_capacity_failure(
                storage.frequency_metadata.size(), memberships.size()));
            return false;
        }
        std::size_t total{};
        for (std::size_t index{}; index < memberships.size(); ++index) {
            const auto next = checked_add_size(total, memberships[index]);
            if (!next) {
                context.failure->emplace(monitoring_storage_capacity_failure(
                    storage.frequency_cpu_storage.capacity(),
                    std::numeric_limits<std::size_t>::max()));
                return false;
            }
            total = *next;
        }
        const auto maximum = static_cast<std::size_t>(
            storage.capacity.maximum_membership_count());
        if (total > maximum) {
            context.failure->emplace(
                monitoring_storage_capacity_failure(maximum, total));
            return false;
        }
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                commit_frequency_partition(*storage.capture_slots,
                                           *storage.classifier_scratch,
                                           memberships)) {
            context.failure->emplace(monitoring_storage_capacity_failure(
                storage.frequency_metadata.size(), memberships.size()));
            return false;
        }
        std::size_t membership_offset{};
        for (std::size_t domain{}; domain < memberships.size(); ++domain) {
            storage.frequency_metadata[domain].cpus =
                std::span{storage.frequency_cpu_storage}.subspan(
                    membership_offset, memberships[domain]);
            membership_offset += memberships[domain];
        }
        storage.frequency_domain_count = memberships.size();
        storage.frequency_partition_committed = true;
        return true;
    };

    if (state.capacity.frequency_requested()) {
        if (!prepared->frequency_discovery)
            return std::unexpected{fail_invariant()};
        auto result = platform.discover_cpu_frequencies_prepared(
            std::move(*prepared->frequency_discovery),
            platform_linux::detail::BoundedCpuFrequencyValidationGate{
                .context = &gate_context, .accept = frequency_gate});
        prepared->frequency_discovery.reset();
        if (!result) {
            if (gate_failure)
                return std::unexpected{fail_after_rollback(
                    std::move(*gate_failure))};
            return std::unexpected{fail_after_rollback(
                map_sysfs_failure(std::move(result.error())))};
        }
        if (result->has_value() != gate_context.frequency_supported)
            return std::unexpected{fail_invariant()};
        if (*result)
            frequency_sources.emplace(std::move(**result));
    } else {
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                commit_frequency_partition(*state.capture_slots,
                                           *state.classifier_scratch,
                                           std::span<const std::size_t>{}))
            return std::unexpected{fail_invariant()};
        state.frequency_domain_count = 0U;
        state.frequency_partition_committed = true;
    }
    if (state.capacity.temperature_requested()) {
        if (!prepared->temperature_discovery)
            return std::unexpected{fail_invariant()};
        auto result = platform.discover_hwmon_temperature_prepared(
            std::move(*prepared->temperature_discovery),
            platform_linux::detail::BoundedHwmonValidationGate{
                .context = &gate_context, .accept = temperature_gate});
        prepared->temperature_discovery.reset();
        if (!result) {
            if (gate_failure)
                return std::unexpected{fail_after_rollback(
                    std::move(*gate_failure))};
            return std::unexpected{fail_after_rollback(
                map_sysfs_failure(std::move(result.error())))};
        }
        if (result->has_value() != gate_context.temperature_supported)
            return std::unexpected{fail_invariant()};
        if (*result)
            temperature_source.emplace(std::move(**result));
    }

    state.temperature_supported = gate_context.temperature_supported;
    state.frequency_supported = gate_context.frequency_supported;
    const auto& policy = state.capacity.admission().monitoring_policy();
    state.temperature_unavailable = state.capacity.temperature_requested() &&
        !gate_context.temperature_supported &&
        policy.disposition(monitoring::MonitoringChannel::temperature) ==
            monitoring::MonitoringChannelDisposition::optional &&
        policy.optional_unsupported() ==
            monitoring::OptionalUnavailableSourcePolicy::warn;
    state.frequency_unavailable = state.capacity.frequency_requested() &&
        !gate_context.frequency_supported &&
        policy.disposition(monitoring::MonitoringChannel::frequency) ==
            monitoring::MonitoringChannelDisposition::optional &&
        policy.optional_unsupported() ==
            monitoring::OptionalUnavailableSourcePolicy::warn;

    if (state.capacity.context_requested()) {
        if (prepared->context_workspaces.size() != state.workers.size())
            return std::unexpected{fail_invariant()};
        for (std::size_t index{}; index < state.workers.size(); ++index) {
            auto source = platform.prepare_perf_context_switch_prepared(
                std::move(prepared->context_workspaces[index]),
                state.workers[index].operational_thread_id(),
                state.capacity.base_page_bytes(),
                state.capacity.perf_record_capacity());
            if (!source) {
                auto primary = PairwiseMonitoringOperationFailure::from_platform(
                    std::move(source.error().primary));
                if (!primary)
                    return std::unexpected{fail_invariant()};
                for (auto& cleanup : source.error().cleanup)
                    if (cleanup &&
                        !append_platform_failure(std::move(*cleanup)))
                        return std::unexpected{fail_invariant()};
                if (rollback())
                    return std::unexpected{fail_invariant()};
                auto failure = PairwiseMonitoringPreparationFailure::operation(
                    std::move(*primary),
                    std::move(prepared->rollback_cleanup));
                if (source_retirement_count != 0U)
                    return std::unexpected{
                        PairwiseMonitoringPreparationFailure::
                            with_source_retirement(
                                std::move(failure),
                                std::move(state.source_retirement),
                                source_retirement_count,
                                frequency_source_retirement_count)};
                return std::unexpected{std::move(failure)};
            }
            if (state.context_sources.size() == state.context_sources.capacity())
                return std::unexpected{fail_after_rollback(
                    monitoring_storage_capacity_failure(
                        state.context_sources.capacity(), index + 1U))};
            state.context_sources.push_back(std::move(*source));
        }
    }

    if (frequency_sources) {
        if (frequency_sources->domain_count() != state.frequency_domain_count)
            return std::unexpected{fail_invariant()};
        std::size_t previous_count{};
        for (std::size_t domain{}; domain < frequency_sources->domain_count();
             ++domain) {
            auto& destination = state.frequency_metadata[domain];
            const auto cpus = frequency_sources->related_cpus(domain);
            const auto driver = frequency_sources->driver(domain);
            const auto governor = frequency_sources->governor(domain);
            const auto ordered = domain == 0U || std::lexicographical_compare(
                state.frequency_metadata[domain - 1U].cpus.begin(),
                state.frequency_metadata[domain - 1U].cpus.end(), cpus.begin(),
                cpus.end());
            if (cpus.empty() || !ordered || cpus.size() != destination.cpus.size() ||
                driver.size() > destination.driver.capacity() ||
                governor.size() > destination.governor.capacity())
                return std::unexpected{fail_invariant()};
            std::ranges::copy(cpus, destination.cpus.begin());
            destination.kind = frequency_sources->kind(domain) ==
                    platform_linux::CpuFrequencyCurrentKind::hardware_current
                ? monitoring::FrequencySourceKind::hardware_current
                : monitoring::FrequencySourceKind::scaling_current;
            destination.driver.assign(driver);
            destination.governor.assign(governor);
            destination.minimum_hz = frequency_sources->minimum_hz(domain);
            destination.maximum_hz = frequency_sources->maximum_hz(domain);
            previous_count += cpus.size();
            if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    initialize_frequency_implementation_ids(
                        *state.capture_slots, domain, destination.kind))
                return std::unexpected{fail_invariant()};
        }
        if (previous_count > state.capacity.maximum_membership_count())
            return std::unexpected{fail_invariant()};
    }

    const auto effective_asynchronous = state.capacity.context_requested() ||
        state.temperature_supported || state.frequency_supported;
    if (!effective_asynchronous)
        state.async.reset();
    if (state.async) {
        if (!prepared->service_affinity_workspace)
            return std::unexpected{fail_invariant()};
        state.retirement_platform.emplace(platform);
        state.async->affinity_workspace.emplace(
            std::move(*prepared->service_affinity_workspace));
        auto* const state_pointer = prepared->storage.get();
        try {
            state.async->service.emplace(
                [state_pointer](std::stop_token token) noexcept {
                    monitoring_service_main(state_pointer, token);
                });
        } catch (const std::exception&) {
            if (rollback())
                return std::unexpected{fail_invariant()};
            return std::unexpected{PairwiseMonitoringPreparationFailure::operation(
                monitoring_preparation_contained(
                    PairwiseFailureReason::contained_exception),
                std::move(prepared->rollback_cleanup),
                std::move(state.source_retirement),
                source_retirement_count,
                frequency_source_retirement_count)};
        } catch (...) {
            if (rollback())
                return std::unexpected{fail_invariant()};
            return std::unexpected{PairwiseMonitoringPreparationFailure::operation(
                monitoring_preparation_contained(
                    PairwiseFailureReason::contained_unknown_failure),
                std::move(prepared->rollback_cleanup),
                std::move(state.source_retirement),
                source_retirement_count,
                frequency_source_retirement_count)};
        }
        state.service_owner = true;
        std::unique_lock lock{state.async->mutex};
        state.async->condition.wait(lock, [&] {
            return state.async->startup_ready ||
                state.async->startup_failure.has_value();
        });
        if (state.async->startup_failure) {
            auto startup = std::move(*state.async->startup_failure);
            state.async->startup_failure.reset();
            lock.unlock();
            const auto ordinary_join = terminalize_monitoring_service(state);
            const auto force = state.async->force;
            const auto force_count = state.async->force_count;
            if (auto* operation =
                    std::get_if<PairwiseMonitoringOperationFailure>(&startup)) {
                if (rollback())
                    return std::unexpected{fail_invariant()};
                auto failed = PairwiseMonitoringPreparationFailure::operation(
                    std::move(*operation),
                    std::move(prepared->rollback_cleanup),
                    std::move(state.source_retirement),
                    source_retirement_count,
                    frequency_source_retirement_count);
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::
                        with_service_terminalization(
                            std::move(failed), ordinary_join, force,
                            force_count)};
            }
            return std::unexpected{fail_invariant()};
        }
    }
    state.temperature_source.reset();
    if (temperature_source)
        state.temperature_source.emplace(std::move(*temperature_source));
    state.frequency_sources.reset();
    if (frequency_sources)
        state.frequency_sources.emplace(std::move(*frequency_sources));
    state.source_retirement_count = 0U;
    if (!state.retirement_platform)
        state.retirement_platform.emplace(platform);
    if (!state.preparation_checkpoint.complete(
            state.capacity.admission().monitoring_policy(), state.workers.size(),
            state.page_workspaces.size(), state.context_sources.size(),
            state.temperature_supported, state.frequency_supported, state.frequency_domain_count))
        std::terminate();
    return PairwiseMonitoringOwningFacade{std::move(prepared->storage)};
}

auto PairwiseMonitoringOwningFacade::retired_preparation_facts() const noexcept
    -> std::optional<MonitoringPreparationFactView> {
    if (!state_) return std::nullopt;
    return MonitoringPreparationFactView::project(MonitoringPreparationFactDomain::source,
        state_->preparation_checkpoint, state_->stopped, state_->capacity.admission(),
        state_->workers, state_->frequency_domain_count, state_.get(),
        [](const void* backing, std::size_t index) noexcept {
            const auto& row = static_cast<const State*>(backing)->frequency_metadata[index];
            return MonitoringPreparationFrequencyView{row.cpus, row.kind, row.driver,
                row.governor, row.minimum_hz, row.maximum_hz};
        });
}

// Private synchronous copy boundary; no active borrow escapes the transaction.
auto PairwiseMonitoringOwningFacade::preparation_graph_facts() const noexcept
    -> std::optional<MonitoringPreparationFactView> {
    if (!state_) return std::nullopt;
    return MonitoringPreparationFactView::project(MonitoringPreparationFactDomain::source,
        state_->preparation_checkpoint, true, state_->capacity.admission(),
        state_->workers, state_->frequency_domain_count, state_.get(),
        [](const void* backing, std::size_t index) noexcept {
            const auto& row = static_cast<const State*>(backing)->frequency_metadata[index];
            return MonitoringPreparationFrequencyView{row.cpus, row.kind, row.driver,
                row.governor, row.minimum_hz, row.maximum_hz};
        });
}

auto PairwiseMonitoringOwningFacade::prepare_storage(
    PairwiseAdmittedMonitoringCapacity capacity,
    std::span<const platform_linux::LinuxThreadId> threads,
    std::optional<platform_linux::detail::BoundedHwmonValidatedOutcome>
        temperature_outcome,
    std::optional<platform_linux::detail::BoundedCpuFrequencyValidatedGraphOutcome>
        frequency_outcome) noexcept
    -> std::expected<PairwiseMonitoringOwningFacade,
                     PairwiseMonitoringPreparationFailure> {
    if (capacity.temperature_requested() != temperature_outcome.has_value() ||
        capacity.frequency_requested() != frequency_outcome.has_value())
        return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure())};
    const auto temperature_supported = temperature_outcome &&
        temperature_outcome->disposition() ==
            platform_linux::detail::BoundedHwmonValidatedDisposition::supported;
    const auto frequency_disposition = frequency_outcome
        ? frequency_outcome->disposition()
        : platform_linux::detail::BoundedCpuFrequencyValidatedDisposition::unsupported;
    const auto memberships = frequency_outcome
        ? frequency_outcome->domain_memberships()
        : std::span<const std::size_t>{};
    return prepare_storage_impl(
        std::move(capacity), threads, temperature_supported,
        frequency_disposition, memberships);
}

auto PairwiseMonitoringOwningFacade::prepare_sources(
    PairwiseAdmittedMonitoringCapacity capacity,
    std::span<const platform_linux::LinuxThreadId> threads,
    const PairwisePlatformLeafFacade& platform,
    platform_linux::detail::BoundedHwmonCapacity temperature_capacity,
    platform_linux::detail::BoundedSysfsOperations temperature_operations,
    platform_linux::detail::BoundedCpuFrequencyCapacity frequency_capacity,
    std::span<const core::LinuxCpuId> present_cpus,
    std::span<const core::LinuxCpuId> worker_cpus,
    platform_linux::detail::BoundedSysfsOperations frequency_operations) noexcept
    -> std::expected<PairwiseMonitoringOwningFacade,
                     PairwiseMonitoringPreparationFailure> {
    return prepare_sources_with_fault(
        std::move(capacity), threads, platform, temperature_capacity,
        temperature_operations, frequency_capacity, present_cpus, worker_cpus,
        frequency_operations, PairwiseMonitoringPreparationFault::none);
}

auto PairwiseMonitoringOwningFacade::prepare_sources_with_fault(
    PairwiseAdmittedMonitoringCapacity capacity,
    std::span<const platform_linux::LinuxThreadId> threads,
    const PairwisePlatformLeafFacade& platform,
    platform_linux::detail::BoundedHwmonCapacity temperature_capacity,
    platform_linux::detail::BoundedSysfsOperations temperature_operations,
    platform_linux::detail::BoundedCpuFrequencyCapacity frequency_capacity,
    std::span<const core::LinuxCpuId> present_cpus,
    std::span<const core::LinuxCpuId> worker_cpus,
    platform_linux::detail::BoundedSysfsOperations frequency_operations,
    PairwiseMonitoringPreparationFault fault) noexcept
    -> std::expected<PairwiseMonitoringOwningFacade,
                     PairwiseMonitoringPreparationFailure> {
    struct Transaction final {
        std::optional<PairwiseAdmittedMonitoringCapacity> capacity;
        std::span<const platform_linux::LinuxThreadId> threads;
        const PairwisePlatformLeafFacade* platform{};
        platform_linux::detail::BoundedHwmonCapacity temperature_capacity{};
        platform_linux::detail::BoundedSysfsOperations temperature_operations{};
        platform_linux::detail::BoundedHwmonValidationGate temperature_gate{};
        std::optional<platform_linux::detail::BoundedCpuFrequencyValidatedGraphOutcome>
            frequency_outcome;
        std::optional<PairwisePreparedTemperatureSource> temperature_source;
        std::optional<PairwisePreparedFrequencySources> frequency_sources;
        std::optional<PairwiseMonitoringOwningFacade> prepared;
        std::optional<PairwiseMonitoringPreparationFailure> failure;
        std::vector<PairwiseMonitoringOperationFailure> rollback_cleanup;
        std::vector<PairwisePerfPreparationWorkspace> context_workspaces;
        std::optional<PairwisePreparedAffinityWorkspace> service_affinity_workspace;
        std::vector<platform_linux::detail::BoundedSysfsDisposition>
            source_retirement;
        std::size_t source_retirement_count{};
        std::size_t frequency_source_retirement_count{};
        bool temperature_supported{};
        bool frequency_supported{};
        bool storage_consumed{};
        PairwiseMonitoringPreparationFault fault{};
        bool mapping_fault_consumed{};
    } transaction{
        .capacity = std::optional<PairwiseAdmittedMonitoringCapacity>{
            std::move(capacity)},
        .threads = threads,
        .platform = &platform,
        .temperature_capacity = temperature_capacity,
        .temperature_operations = temperature_operations,
        .temperature_gate = {},
        .frequency_outcome = {},
        .temperature_source = {},
        .frequency_sources = {},
        .prepared = {},
        .failure = {},
        .rollback_cleanup = {},
        .context_workspaces = {},
        .service_affinity_workspace = {},
        .source_retirement = {},
        .source_retirement_count = 0U,
        .frequency_source_retirement_count = 0U,
        .temperature_supported = false,
        .frequency_supported = false,
        .storage_consumed = false,
        .fault = fault,
        .mapping_fault_consumed = false};

    const auto prepare = [](Transaction& value,
                            std::optional<
                                platform_linux::detail::BoundedHwmonValidatedOutcome>
                                temperature_outcome) noexcept -> bool {
        if (value.storage_consumed || !value.capacity ||
            (value.capacity->frequency_requested() && !value.frequency_outcome)) {
            value.failure.emplace(PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure()));
            return false;
        }
        value.storage_consumed = true;
        if (temperature_outcome)
            value.temperature_supported = temperature_outcome->disposition() ==
                platform_linux::detail::BoundedHwmonValidatedDisposition::supported;
        if (value.frequency_outcome)
            value.frequency_supported = value.frequency_outcome->disposition() ==
                platform_linux::detail::BoundedCpuFrequencyValidatedDisposition::supported;
        auto result = PairwiseMonitoringOwningFacade::prepare_storage(
            std::move(*value.capacity), value.threads,
            std::move(temperature_outcome), std::move(value.frequency_outcome));
        value.capacity.reset();
        if (!result) {
            value.failure.emplace(std::move(result.error()));
            return false;
        }
        value.prepared.emplace(std::move(*result));
        return true;
    };

    const auto temperature_gate = +[](
        void* raw,
        platform_linux::detail::BoundedHwmonValidatedOutcome&& outcome) noexcept
        -> bool {
        auto& value = *static_cast<Transaction*>(raw);
        if (value.storage_consumed || !value.capacity) {
            value.failure.emplace(PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure()));
            return false;
        }
        value.temperature_supported = outcome.disposition() ==
            platform_linux::detail::BoundedHwmonValidatedDisposition::supported;
        value.storage_consumed = true;
        if (value.capacity->frequency_requested() && !value.frequency_outcome) {
            value.failure.emplace(PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure()));
            return false;
        }
        auto result = PairwiseMonitoringOwningFacade::prepare_storage(
            std::move(*value.capacity), value.threads,
            std::optional<platform_linux::detail::BoundedHwmonValidatedOutcome>{
                std::move(outcome)},
            std::move(value.frequency_outcome));
        value.capacity.reset();
        if (!result) {
            value.failure.emplace(std::move(result.error()));
            return false;
        }
        value.prepared.emplace(std::move(*result));
        return true;
    };

    const auto frequency_gate = +[](
        void* raw,
        platform_linux::detail::BoundedCpuFrequencyValidatedGraphOutcome&& outcome)
        noexcept -> bool {
        auto& value = *static_cast<Transaction*>(raw);
        if (value.frequency_outcome || value.storage_consumed || !value.capacity) {
            value.failure.emplace(PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure()));
            return false;
        }
        value.frequency_supported = outcome.disposition() ==
            platform_linux::detail::BoundedCpuFrequencyValidatedDisposition::supported;
        value.frequency_outcome.emplace(std::move(outcome));
        if (!value.capacity->temperature_requested()) {
            value.storage_consumed = true;
            auto result = PairwiseMonitoringOwningFacade::prepare_storage(
                std::move(*value.capacity), value.threads, std::nullopt,
                std::move(value.frequency_outcome));
            value.capacity.reset();
            if (!result) {
                value.failure.emplace(std::move(result.error()));
                return false;
            }
            value.prepared.emplace(std::move(*result));
            return true;
        }
        auto temperature = value.platform->discover_hwmon_temperature(
            value.temperature_capacity, value.temperature_operations,
            value.temperature_gate);
        if (!temperature) {
            if (!value.failure)
                value.failure.emplace(monitoring_preparation_failure(
                    temperature.error()));
            return false;
        }
        if (temperature->has_value() != value.temperature_supported) {
            if (*temperature)
                value.temperature_source.emplace(std::move(**temperature));
            value.failure.emplace(PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure()));
            return false;
        }
        if (*temperature)
            value.temperature_source.emplace(std::move(**temperature));
        if (value.fault ==
            PairwiseMonitoringPreparationFault::mismatch_temperature_owner) {
            value.temperature_supported = !value.temperature_supported;
            value.failure.emplace(PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure()));
            return false;
        }
        return value.prepared.has_value() && !value.failure;
    };

    const auto append_platform_failure = [](
        Transaction& value, PairwisePlatformFailure failure) noexcept -> bool {
        if (!value.mapping_fault_consumed &&
            (value.fault ==
                 PairwiseMonitoringPreparationFault::reject_platform_mapping ||
             value.fault ==
                 PairwiseMonitoringPreparationFault::exhaust_typed_cleanup)) {
            value.mapping_fault_consumed = true;
            return false;
        }
        auto mapped = PairwiseMonitoringOperationFailure::from_platform(
            std::move(failure));
        if (!mapped || value.rollback_cleanup.size() ==
                           value.rollback_cleanup.capacity())
            return false;
        value.rollback_cleanup.push_back(std::move(*mapped));
        return true;
    };
    const auto rollback_all = [&platform, &append_platform_failure](
        Transaction& value) noexcept -> bool {
        bool invariant{};
        if (value.prepared) {
            auto& contexts = value.prepared->state_->context_sources;
            for (auto prior = contexts.rbegin(); prior != contexts.rend(); ++prior) {
                auto retired = platform.retire_perf_context_switch(
                    std::move(*prior));
                if (retired.primary &&
                    !append_platform_failure(value, std::move(*retired.primary)))
                    invariant = true;
                if (retired.cleanup &&
                    !append_platform_failure(value, std::move(*retired.cleanup)))
                    invariant = true;
            }
            contexts.clear();
        }
        const auto remaining = [&value]() noexcept -> std::span<
            platform_linux::detail::BoundedSysfsDisposition> {
            if (value.fault ==
                PairwiseMonitoringPreparationFault::exhaust_source_disposition)
                return {};
            if (value.source_retirement_count >= value.source_retirement.size())
                return {};
            return std::span{value.source_retirement}.subspan(
                value.source_retirement_count);
        };
        if (value.frequency_sources) {
            const auto available = remaining().size();
            const auto retired = platform.retire_cpu_frequencies(
                std::move(*value.frequency_sources), remaining());
            value.frequency_sources.reset();
            if (retired > available) {
                invariant = true;
                value.source_retirement_count = value.source_retirement.size();
            } else {
                value.source_retirement_count += retired;
            }
            value.frequency_source_retirement_count =
                value.source_retirement_count;
        }
        if (value.temperature_source) {
            const auto available = remaining().size();
            const auto retired = platform.retire_hwmon_temperature(
                std::move(*value.temperature_source), remaining());
            value.temperature_source.reset();
            if (retired > available) {
                invariant = true;
                value.source_retirement_count = value.source_retirement.size();
            } else {
                value.source_retirement_count += retired;
            }
        }
        return invariant;
    };
    const auto invariant_after_rollback = [&rollback_all](Transaction& value) noexcept {
        static_cast<void>(rollback_all(value));
        auto failure = PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure());
        if (value.source_retirement_count != 0U)
            return PairwiseMonitoringPreparationFailure::with_source_retirement(
                std::move(failure), std::move(value.source_retirement),
                value.source_retirement_count,
                value.frequency_source_retirement_count);
        return failure;
    };

    try {
        const auto rollback = checked_size(
            transaction.capacity->preparation_rollback_values());
        const auto maximum_domains = checked_size(
            transaction.capacity->maximum_domain_count());
        if (!rollback || *rollback == 0U || !maximum_domains ||
            *maximum_domains >
                (std::numeric_limits<std::size_t>::max() - 1U) / 2U)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        transaction.rollback_cleanup.reserve(*rollback - 1U);
        if (transaction.capacity->asynchronous_requested()) {
            if (!transaction.capacity->service_cpu())
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::operation(
                        monitoring_missing_service_binding_failure())};
            const auto affinity_capacity = checked_size(
                transaction.capacity->kernel_cpu_count());
            if (!affinity_capacity)
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::runtime(
                        local_preparation_failure())};
            auto workspace = platform.prepare_affinity_workspace(
                *affinity_capacity);
            if (!workspace) {
                auto failure =
                    PairwiseMonitoringOperationFailure::from_service_platform(
                        std::move(workspace.error()));
                if (!failure)
                    return std::unexpected{
                        PairwiseMonitoringPreparationFailure::runtime(
                            local_preparation_failure())};
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::operation(
                        std::move(*failure))};
            }
            transaction.service_affinity_workspace.emplace(
                std::move(*workspace));
        }
        if (transaction.capacity->context_requested()) {
            transaction.context_workspaces.reserve(threads.size());
            for (std::size_t index{}; index < threads.size(); ++index) {
                auto workspace = platform.prepare_perf_context_switch_workspace();
                if (!workspace) {
                    auto primary =
                        PairwiseMonitoringOperationFailure::from_platform(
                            std::move(workspace.error()));
                    if (!primary)
                        return std::unexpected{
                            PairwiseMonitoringPreparationFailure::runtime(
                                local_preparation_failure())};
                    return std::unexpected{
                        PairwiseMonitoringPreparationFailure::operation(
                            std::move(*primary))};
                }
                transaction.context_workspaces.push_back(
                    std::move(*workspace));
            }
        }
        const auto source_retirement_capacity = checked_add_size(
            2U * *maximum_domains + 1U, 3U);
        if (!source_retirement_capacity)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        transaction.source_retirement.resize(*source_retirement_capacity);
        transaction.temperature_gate = {
            .context = &transaction, .accept = temperature_gate};

        if (transaction.capacity->frequency_requested()) {
            auto frequency = platform.discover_cpu_frequencies(
                frequency_capacity, present_cpus, worker_cpus,
                frequency_operations,
                platform_linux::detail::BoundedCpuFrequencyValidationGate{
                    .context = &transaction, .accept = frequency_gate});
            if (!frequency) {
                const auto rollback_invariant = rollback_all(transaction);
                std::optional<PairwiseMonitoringPreparationFailure> mapped;
                if (transaction.failure) {
                    mapped.emplace(std::move(*transaction.failure));
                } else {
                    auto primary = PairwiseMonitoringOperationFailure::from_platform(
                        std::move(frequency.error().primary));
                    if (!primary)
                        return std::unexpected{
                            invariant_after_rollback(transaction)};
                    for (auto& value : frequency.error().cleanup) {
                        if (!append_platform_failure(transaction,
                                                     std::move(value)))
                            return std::unexpected{
                                invariant_after_rollback(transaction)};
                    }
                    mapped.emplace(PairwiseMonitoringPreparationFailure::operation(
                        std::move(*primary),
                        std::move(transaction.rollback_cleanup)));
                }
                if (rollback_invariant)
                    return std::unexpected{
                        invariant_after_rollback(transaction)};
                if (transaction.source_retirement_count == 0U)
                    return std::unexpected{std::move(*mapped)};
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::with_source_retirement(
                        std::move(*mapped),
                        std::move(transaction.source_retirement),
                        transaction.source_retirement_count,
                        transaction.frequency_source_retirement_count)};
            }
            if (frequency->has_value() != transaction.frequency_supported) {
                if (*frequency)
                    transaction.frequency_sources.emplace(
                        std::move(**frequency));
                return std::unexpected{invariant_after_rollback(transaction)};
            }
            if (*frequency)
                transaction.frequency_sources.emplace(std::move(**frequency));
            if (transaction.fault ==
                PairwiseMonitoringPreparationFault::mismatch_frequency_owner) {
                transaction.frequency_supported =
                    !transaction.frequency_supported;
                return std::unexpected{invariant_after_rollback(transaction)};
            }
        } else if (transaction.capacity->temperature_requested()) {
            auto temperature = platform.discover_hwmon_temperature(
                temperature_capacity, temperature_operations,
                platform_linux::detail::BoundedHwmonValidationGate{
                    .context = &transaction, .accept = temperature_gate});
            if (!temperature) {
                static_cast<void>(rollback_all(transaction));
                if (transaction.failure)
                    return std::unexpected{std::move(*transaction.failure)};
                return std::unexpected{monitoring_preparation_failure(
                    std::move(temperature.error()))};
            }
            if (temperature->has_value() != transaction.temperature_supported) {
                if (*temperature)
                    transaction.temperature_source.emplace(
                        std::move(**temperature));
                return std::unexpected{invariant_after_rollback(transaction)};
            }
            if (*temperature)
                transaction.temperature_source.emplace(std::move(**temperature));
            if (transaction.fault ==
                PairwiseMonitoringPreparationFault::mismatch_temperature_owner)
                return std::unexpected{invariant_after_rollback(transaction)};
        } else {
            auto result = prepare(transaction, std::nullopt);
            if (!result) {
                if (transaction.failure)
                    return std::unexpected{std::move(*transaction.failure)};
                return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                    local_preparation_failure())};
            }
        }
        if (transaction.failure)
            return std::unexpected{invariant_after_rollback(transaction)};
        if (!transaction.prepared || !transaction.storage_consumed)
            return std::unexpected{invariant_after_rollback(transaction)};
        if (transaction.fault ==
            PairwiseMonitoringPreparationFault::contained_after_owner)
            throw 0;

        if (transaction.prepared->state_->capacity.context_requested()) {
            std::size_t worker_index{};
            for (const auto& worker : transaction.prepared->state_->workers) {
                if (worker_index >= transaction.context_workspaces.size())
                    return std::unexpected{
                        invariant_after_rollback(transaction)};
                auto source = platform.prepare_perf_context_switch_prepared(
                    std::move(transaction.context_workspaces[worker_index++]),
                    worker.operational_thread_id(),
                    transaction.prepared->state_->capacity.base_page_bytes(),
                    transaction.prepared->state_->capacity.perf_record_capacity());
                if (source) {
                    transaction.prepared->state_->context_sources.push_back(
                        std::move(*source));
                    if ((transaction.fault ==
                             PairwiseMonitoringPreparationFault::
                                 reject_platform_mapping ||
                         transaction.fault ==
                             PairwiseMonitoringPreparationFault::
                                 exhaust_typed_cleanup ||
                         transaction.fault ==
                             PairwiseMonitoringPreparationFault::
                                 exhaust_source_disposition) &&
                        transaction.prepared->state_->context_sources.size() == 1U)
                        throw 0;
                    continue;
                }
                auto primary = PairwiseMonitoringOperationFailure::from_platform(
                    std::move(source.error().primary));
                bool invariant = !primary.has_value();
                for (auto& value : source.error().cleanup) {
                    if (value && !append_platform_failure(
                                     transaction, std::move(*value)))
                        invariant = true;
                }
                invariant = rollback_all(transaction) || invariant;
                if (invariant)
                    return std::unexpected{
                        invariant_after_rollback(transaction)};
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::operation(
                        std::move(*primary),
                        std::move(transaction.rollback_cleanup),
                        std::move(transaction.source_retirement),
                        transaction.source_retirement_count,
                        transaction.frequency_source_retirement_count)};
            }
        }
        if (transaction.frequency_sources) {
            auto& sources = *transaction.frequency_sources;
            auto& prepared_state = *transaction.prepared->state_;
            if (sources.domain_count() !=
                prepared_state.frequency_domain_count)
                return std::unexpected{invariant_after_rollback(transaction)};
            std::size_t membership_count{};
            std::size_t expected_membership_count{};
            for (std::size_t domain{}; domain < sources.domain_count();
                 ++domain) {
                auto& destination =
                    prepared_state.frequency_metadata[domain];
                const auto cpus = sources.related_cpus(domain);
                const auto driver = sources.driver(domain);
                const auto governor = sources.governor(domain);
                const auto ordered = domain == 0U ||
                    std::lexicographical_compare(
                        prepared_state.frequency_metadata[domain - 1U].cpus.begin(),
                        prepared_state.frequency_metadata[domain - 1U].cpus.end(),
                        cpus.begin(), cpus.end());
                if (cpus.empty() || !ordered ||
                    cpus.size() != destination.cpus.size() ||
                    driver.size() > destination.driver.capacity() ||
                    governor.size() > destination.governor.capacity())
                    return std::unexpected{
                        invariant_after_rollback(transaction)};
                std::ranges::copy(cpus, destination.cpus.begin());
                destination.kind =
                    sources.kind(domain) ==
                            platform_linux::CpuFrequencyCurrentKind::
                                hardware_current
                        ? monitoring::FrequencySourceKind::hardware_current
                        : monitoring::FrequencySourceKind::scaling_current;
                destination.driver.assign(driver);
                destination.governor.assign(governor);
                destination.minimum_hz = sources.minimum_hz(domain);
                destination.maximum_hz = sources.maximum_hz(domain);
                const auto next = checked_add_size(membership_count, cpus.size());
                const auto expected = checked_add_size(
                    expected_membership_count, destination.cpus.size());
                if (!next || !expected ||
                    !monitoring::detail::PairwisePreparedCaptureStorageAccess::
                        initialize_frequency_implementation_ids(
                            *prepared_state.capture_slots, domain,
                            destination.kind))
                    return std::unexpected{
                        invariant_after_rollback(transaction)};
                membership_count = *next;
                expected_membership_count = *expected;
            }
            if (membership_count != expected_membership_count)
                return std::unexpected{invariant_after_rollback(transaction)};
        }
        if (transaction.prepared->state_->async) {
            try {
                auto& prepared_state = *transaction.prepared->state_;
                prepared_state.retirement_platform.emplace(platform);
                if (!transaction.service_affinity_workspace)
                    return std::unexpected{
                        invariant_after_rollback(transaction)};
                prepared_state.async->affinity_workspace.emplace(
                    std::move(*transaction.service_affinity_workspace));
                auto& async = *prepared_state.async;
                async.missing_placement_operation =
                    transaction.fault == PairwiseMonitoringPreparationFault::
                                             missing_service_placement ||
                    transaction.fault == PairwiseMonitoringPreparationFault::
                                             missing_service_placement_join_native ||
                    transaction.fault == PairwiseMonitoringPreparationFault::
                                             missing_service_placement_join_contained_standard ||
                    transaction.fault == PairwiseMonitoringPreparationFault::
                                             missing_service_placement_join_contained_unknown ||
                    transaction.fault == PairwiseMonitoringPreparationFault::
                                             missing_service_placement_join_forced_second ||
                    transaction.fault == PairwiseMonitoringPreparationFault::
                                             missing_service_placement_join_terminal;
                const auto set_join_script = [&](auto... values) noexcept {
                    const std::array script{values...};
                    std::ranges::copy(script, async.join_script.begin());
                    async.join_script_count = script.size();
                };
                switch (transaction.fault) {
                case PairwiseMonitoringPreparationFault::
                    missing_service_placement_join_native:
                    set_join_script(
                        PairwiseMonitoringServiceJoinFault::native_failure,
                        PairwiseMonitoringServiceJoinFault::success);
                    break;
                case PairwiseMonitoringPreparationFault::
                    missing_service_placement_join_contained_standard:
                    set_join_script(
                        PairwiseMonitoringServiceJoinFault::contained_standard,
                        PairwiseMonitoringServiceJoinFault::success);
                    break;
                case PairwiseMonitoringPreparationFault::
                    missing_service_placement_join_contained_unknown:
                    set_join_script(
                        PairwiseMonitoringServiceJoinFault::contained_unknown,
                        PairwiseMonitoringServiceJoinFault::success);
                    break;
                case PairwiseMonitoringPreparationFault::
                    missing_service_placement_join_forced_second:
                    set_join_script(
                        PairwiseMonitoringServiceJoinFault::native_failure,
                        PairwiseMonitoringServiceJoinFault::contained_standard,
                        PairwiseMonitoringServiceJoinFault::success);
                    break;
                case PairwiseMonitoringPreparationFault::
                    missing_service_placement_join_terminal:
                    set_join_script(
                        PairwiseMonitoringServiceJoinFault::native_failure,
                        PairwiseMonitoringServiceJoinFault::contained_standard,
                        PairwiseMonitoringServiceJoinFault::contained_unknown);
                    break;
                default:
                    break;
                }
                if (transaction.fault ==
                    PairwiseMonitoringPreparationFault::service_construction)
                    throw std::runtime_error{"injected Monitoring service construction"};
                auto* const prepared_state_pointer =
                    transaction.prepared->state_.get();
                prepared_state.async->service.emplace(
                    [prepared_state_pointer](std::stop_token token) noexcept {
                        monitoring_service_main(prepared_state_pointer, token);
                    });
                prepared_state.service_owner = true;
                std::unique_lock lock{prepared_state.async->mutex};
                prepared_state.async->condition.wait(lock, [&] {
                    return prepared_state.async->startup_ready ||
                           prepared_state.async->startup_failure.has_value();
                });
                if (prepared_state.async->startup_failure) {
                    auto startup =
                        std::move(*prepared_state.async->startup_failure);
                    prepared_state.async->startup_failure.reset();
                    lock.unlock();
                    const auto ordinary_join =
                        terminalize_monitoring_service(prepared_state);
                    const auto force_dispositions =
                        prepared_state.async->force;
                    const auto force_count =
                        prepared_state.async->force_count;
                    const bool invariant = rollback_all(transaction);
                    if (auto* operation =
                            std::get_if<PairwiseMonitoringOperationFailure>(
                                &startup);
                        operation && !invariant)
                        return std::unexpected{
                            PairwiseMonitoringPreparationFailure::
                                with_service_terminalization(
                                    PairwiseMonitoringPreparationFailure::operation(
                                        std::move(*operation),
                                        std::move(transaction.rollback_cleanup),
                                        std::move(transaction.source_retirement),
                                        transaction.source_retirement_count,
                                        transaction.frequency_source_retirement_count),
                                    ordinary_join, force_dispositions,
                                    force_count)};
                    return std::unexpected{
                        invariant_after_rollback(transaction)};
                }
            } catch (const std::exception&) {
                static_cast<void>(rollback_all(transaction));
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::operation(
                        monitoring_preparation_contained(
                            PairwiseFailureReason::contained_exception),
                        std::move(transaction.rollback_cleanup),
                        std::move(transaction.source_retirement),
                        transaction.source_retirement_count,
                        transaction.frequency_source_retirement_count)};
            } catch (...) {
                static_cast<void>(rollback_all(transaction));
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::operation(
                        monitoring_preparation_contained(
                            PairwiseFailureReason::contained_unknown_failure),
                        std::move(transaction.rollback_cleanup),
                        std::move(transaction.source_retirement),
                        transaction.source_retirement_count,
                        transaction.frequency_source_retirement_count)};
            }
        }
        transaction.prepared->state_->temperature_source.reset();
        if (transaction.temperature_source)
            transaction.prepared->state_->temperature_source.emplace(
                std::move(*transaction.temperature_source));
        transaction.prepared->state_->frequency_sources.reset();
        if (transaction.frequency_sources)
            transaction.prepared->state_->frequency_sources.emplace(
                std::move(*transaction.frequency_sources));
        transaction.prepared->state_->source_retirement =
            std::move(transaction.source_retirement);
        transaction.prepared->state_->source_retirement_count = 0U;
        if (!transaction.prepared->state_->retirement_platform)
            transaction.prepared->state_->retirement_platform.emplace(platform);
        return std::move(*transaction.prepared);
    } catch (...) {
        return std::unexpected{invariant_after_rollback(transaction)};
    }
}

auto PairwiseMonitoringOwningFacade::prepare_storage_impl(
    PairwiseAdmittedMonitoringCapacity capacity,
    std::span<const platform_linux::LinuxThreadId> threads,
    bool temperature_supported,
    platform_linux::detail::BoundedCpuFrequencyValidatedDisposition
        frequency_disposition,
    std::span<const std::size_t> frequency_domain_memberships,
    bool bind_workers, bool maximum_layout) noexcept
    -> std::expected<PairwiseMonitoringOwningFacade,
                     PairwiseMonitoringPreparationFailure> {
    try {
        const auto worker_count = checked_size(capacity.workers());
        if (!worker_count || (bind_workers && threads.size() != *worker_count) ||
            capacity.admission().worker_cpus().size() != *worker_count)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        const auto& policy = capacity.admission().monitoring_policy();
        const auto unsupported_is_failure = [&](
            monitoring::MonitoringChannel channel, bool supported) noexcept {
            if (supported)
                return false;
            const auto authored = policy.disposition(channel);
            return authored == monitoring::MonitoringChannelDisposition::required ||
                (authored == monitoring::MonitoringChannelDisposition::optional &&
                 policy.optional_unsupported() ==
                     monitoring::OptionalUnavailableSourcePolicy::fail);
        };
        if (!maximum_layout && capacity.temperature_requested() &&
            unsupported_is_failure(monitoring::MonitoringChannel::temperature,
                                   temperature_supported))
            return std::unexpected{
                PairwiseMonitoringPreparationFailure::operation(
                    monitoring_required_source_unsupported(
                        monitoring::MonitoringChannel::temperature))};
        const auto frequency_supported = frequency_disposition ==
            platform_linux::detail::BoundedCpuFrequencyValidatedDisposition::supported;
        if (!maximum_layout && capacity.frequency_requested() &&
            unsupported_is_failure(monitoring::MonitoringChannel::frequency,
                                   frequency_supported))
            return std::unexpected{
                PairwiseMonitoringPreparationFailure::operation(
                    monitoring_required_source_unsupported(
                        monitoring::MonitoringChannel::frequency))};
        const auto unsupported_is_warning = [&](
            monitoring::MonitoringChannel channel, bool supported) noexcept {
            return !supported &&
                policy.disposition(channel) ==
                    monitoring::MonitoringChannelDisposition::optional &&
                policy.optional_unsupported() ==
                    monitoring::OptionalUnavailableSourcePolicy::warn;
        };
        const auto temperature_unavailable = unsupported_is_warning(
            monitoring::MonitoringChannel::temperature, temperature_supported);
        const auto frequency_unavailable = unsupported_is_warning(
            monitoring::MonitoringChannel::frequency, frequency_supported);
        auto state = std::make_unique<State>(std::move(capacity));
        state->workers.reserve(*worker_count);
        if (bind_workers)
            for (std::size_t index{}; index < *worker_count; ++index) {
                state->workers.push_back(monitoring::MonitoringWorkerScope::create(
                    core::WorkerId{static_cast<std::uint32_t>(index)}, threads[index],
                    state->capacity.admission().worker_cpus()[index]));
            }
        const auto captures = checked_size(state->capacity.capture_slots());
        const auto rounds = checked_size(state->capacity.round_capacity());
        const auto events = checked_size(state->capacity.perf_record_capacity());
        const auto base_page = checked_size(state->capacity.base_page_bytes());
        const auto maximum_domains = checked_size(
            state->capacity.maximum_domain_count());
        const auto maximum_memberships = checked_size(
            state->capacity.maximum_membership_count());
        const auto page_sources = checked_size(state->capacity.slot(
            PairwiseMonitoringCapacitySlot::persistent_page_sources));
        const auto context_sources = checked_size(state->capacity.slot(
            PairwiseMonitoringCapacitySlot::persistent_context_sources));
        const auto persistent_cpu_ids = checked_size(state->capacity.slot(
            PairwiseMonitoringCapacitySlot::persistent_frequency_cpu_ids));
        const auto rollback = checked_size(
            state->capacity.preparation_rollback_values());
        const auto completion = checked_size(
            state->capacity.completion_abort_values());
        const auto stop = checked_size(state->capacity.whole_stop_values());
        if (!captures || !rounds || !events || !base_page || !maximum_domains ||
            !maximum_memberships || !page_sources || !context_sources ||
            !persistent_cpu_ids || !rollback || !completion || !stop)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        if ((!state->capacity.frequency_requested() &&
             (frequency_supported || !frequency_domain_memberships.empty())) ||
            (state->capacity.frequency_requested() &&
             frequency_disposition !=
                 platform_linux::detail::BoundedCpuFrequencyValidatedDisposition::supported &&
             !frequency_domain_memberships.empty()) ||
            frequency_domain_memberships.size() > *maximum_domains)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        if (frequency_supported && frequency_domain_memberships.empty())
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        std::size_t membership_count{};
        for (const auto membership : frequency_domain_memberships) {
            if (membership == 0U)
                return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                    local_preparation_failure())};
            const auto next = checked_add_size(membership_count, membership);
            if (!next)
                return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                    local_preparation_failure())};
            membership_count = *next;
        }
        if (membership_count > *maximum_memberships)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        const auto frequency_domain_capacity =
            maximum_layout && state->capacity.frequency_requested()
            ? *maximum_domains
            : frequency_domain_memberships.size();
        const auto frequency_membership_capacity =
            maximum_layout && state->capacity.frequency_requested()
            ? *maximum_memberships
            : membership_count;
        const auto rounds_plus_three = checked_add_size(*rounds, 3U);
        const auto nested_per_capture = rounds_plus_three
            ? checked_product_size(frequency_membership_capacity,
                                   *rounds_plus_three)
            : std::nullopt;
        const auto nested_total = nested_per_capture
            ? checked_product_size(*captures, *nested_per_capture)
            : std::nullopt;
        const auto admitted_nested = checked_size(state->capacity.slot(
            PairwiseMonitoringCapacitySlot::frequency_nested_cpu_ids));
        if (!rounds_plus_three || !nested_total || !admitted_nested ||
            (!maximum_layout && *nested_total > *admitted_nested))
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};

        const monitoring::detail::PairwisePreparedCaptureDimensions dimensions{
            .captures = *captures,
            .workers = *worker_count,
            .rounds = *rounds,
            .events_per_drain = *events,
            .base_page_bytes = *base_page,
            .page = state->capacity.page_requested(),
            .context = state->capacity.context_requested(),
            .temperature = maximum_layout ? state->capacity.temperature_requested()
                                          : temperature_supported,
            .frequency = maximum_layout ? state->capacity.frequency_requested()
                                        : frequency_supported};
        state->capture_slots.emplace(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::prepare_slots(
                dimensions, frequency_domain_capacity,
                frequency_membership_capacity));
        state->capture_registrations =
            std::shared_ptr<PairwiseMonitoringCaptureRegistration[]>{
                new PairwiseMonitoringCaptureRegistration[*captures],
                std::default_delete<PairwiseMonitoringCaptureRegistration[]>{}};
        state->capture_registration_count = *captures;
        state->classifier_scratch.emplace(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::prepare_scratch(
                dimensions, frequency_domain_capacity));
        state->page_workspaces.resize(*page_sources);
        state->context_sources.reserve(*context_sources);
        state->context_drain_storage.reserve(*context_sources);
        for (std::size_t index{}; index < *context_sources; ++index)
            state->context_drain_storage.push_back(
                PairwisePerfDrainStorage::prepare(
                    static_cast<std::size_t>(state->capacity.perf_record_capacity())));
        state->context_coverage_start.resize(
            state->capacity.context_requested() ? *worker_count : 0U,
            platform_linux::RawMonotonicTimestamp::from_nanoseconds(0U));
        state->context_coverage_end.resize(
            state->capacity.context_requested() ? *worker_count : 0U,
            platform_linux::RawMonotonicTimestamp::from_nanoseconds(0U));
        state->context_completeness.resize(
            state->capacity.context_requested() ? *worker_count : 0U,
            monitoring::ContextSwitchCaptureCompleteness::complete);
        state->frequency_metadata.resize(frequency_domain_capacity);
        state->frequency_cpu_storage.assign(frequency_membership_capacity,
                                            core::LinuxCpuId{0U});
        for (std::size_t domain{}; domain < frequency_domain_capacity; ++domain) {
            state->frequency_metadata[domain].driver.reserve(*base_page);
            state->frequency_metadata[domain].governor.reserve(*base_page);
        }
        if (temperature_supported ||
            (maximum_layout && state->capacity.temperature_requested()))
            state->temperature_workspace.emplace(
                PairwiseSysfsReadWorkspace::prepare(*base_page));
        state->frequency_workspaces.reserve(frequency_domain_capacity);
        for (std::size_t domain{}; domain < frequency_domain_capacity; ++domain)
            state->frequency_workspaces.push_back(
                PairwiseSysfsReadWorkspace::prepare(*base_page));
        const auto actual_persistent_cpu_ids =
            checked_product_size(2U, frequency_membership_capacity);
        if (!actual_persistent_cpu_ids ||
            (!maximum_layout && *actual_persistent_cpu_ids > *persistent_cpu_ids))
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        state->rollback_returned.resize(*rollback);
        state->completion_returned.resize(*completion);
        state->stop_returned.resize(*stop);
        const auto page_before = checked_size(state->capacity.slot(
            PairwiseMonitoringCapacitySlot::raw_page_before));
        const auto page_after = checked_size(state->capacity.slot(
            PairwiseMonitoringCapacitySlot::raw_page_after));
        const auto page_dispositions = page_before && page_after
            ? checked_add_size(*page_before, *page_after)
            : std::nullopt;
        if (!page_dispositions)
            return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
                local_preparation_failure())};
        state->page_read_dispositions.resize(*page_dispositions);
        const auto effective_asynchronous = maximum_layout
            ? state->capacity.asynchronous_requested()
            : state->capacity.context_requested() || temperature_supported ||
                frequency_supported;
        if (effective_asynchronous) {
            const auto service_cpu = state->capacity.service_cpu();
            if (!service_cpu)
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::operation(
                        monitoring_missing_service_binding_failure())};
            state->async.emplace();
            state->async->service_cpu = *service_cpu;
        }
        state->temperature_supported = temperature_supported;
        state->frequency_supported = frequency_supported;
        state->temperature_unavailable = maximum_layout ? false
                                                        : temperature_unavailable;
        state->frequency_unavailable = maximum_layout ? false
                                                      : frequency_unavailable;
        if (!maximum_layout) {
            if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    commit_frequency_partition(
                        *state->capture_slots, *state->classifier_scratch,
                        frequency_domain_memberships))
                return std::unexpected{
                    PairwiseMonitoringPreparationFailure::runtime(
                        local_preparation_failure())};
            std::size_t membership_offset{};
            for (std::size_t domain{};
                 domain < frequency_domain_memberships.size(); ++domain) {
                state->frequency_metadata[domain].cpus =
                    std::span{state->frequency_cpu_storage}.subspan(
                        membership_offset,
                        frequency_domain_memberships[domain]);
                membership_offset += frequency_domain_memberships[domain];
            }
            state->frequency_domain_count =
                frequency_domain_memberships.size();
            state->frequency_partition_committed = true;
        }
        return PairwiseMonitoringOwningFacade{std::move(state)};
    } catch (const std::exception&) {
        return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure())};
    } catch (...) {
        return std::unexpected{PairwiseMonitoringPreparationFailure::runtime(
            local_preparation_failure())};
    }
}

auto PairwiseMonitoringOwningFacade::capacity() const noexcept
    -> const PairwiseAdmittedMonitoringCapacity& { return state_->capacity; }
auto PairwiseMonitoringOwningFacade::workers() const noexcept
    -> std::span<const monitoring::MonitoringWorkerScope> { return state_->workers; }
auto PairwiseMonitoringOwningFacade::capture_slot_count() const noexcept
    -> std::size_t { return state_->capture_slots->size(); }
auto PairwiseMonitoringOwningFacade::available_capture_slot_count() const noexcept
    -> std::size_t { return state_->capture_slots->available(); }
auto PairwiseMonitoringOwningFacade::take_capture_slot(std::size_t index) noexcept
    -> std::optional<PairwiseMonitoringTerminalCapture> {
    auto slot = monitoring::detail::PairwisePreparedCaptureStorageAccess::take(
        *state_->capture_slots, index);
    if (!slot)
        return std::nullopt;
    return PairwiseMonitoringTerminalCapture{std::move(*slot)};
}

auto PairwiseMonitoringOwningFacade::begin_capture(
    const PairwisePlatformLeafFacade& platform) noexcept
    -> std::expected<PairwiseMonitoringActiveCapture,
                     PairwiseMonitoringCaptureFailure> {
    if (!state_ || state_->stopped || state_->capture_active ||
        state_->capture_sources_terminal || state_->service_terminal ||
        state_->active_slot)
        return std::unexpected{monitoring_driver_transition_failure(
            PairwiseFailureCode::monitoring_invalid_state,
            PairwiseFailureReason::begin_capture,
            PairwiseLifecyclePhase::begin_capture)};
    if (state_->generation == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected{monitoring_driver_transition_failure(
            PairwiseFailureCode::monitoring_driver_invariant_failed,
            PairwiseFailureReason::generation_exhausted,
            PairwiseLifecyclePhase::begin_capture)};
    if (state_->next_capture_slot >= state_->capture_slots->size())
        return std::unexpected{monitoring_driver_transition_failure(
            PairwiseFailureCode::monitoring_invalid_state,
            PairwiseFailureReason::begin_capture,
            PairwiseLifecyclePhase::begin_capture)};
    const auto capture_index = state_->next_capture_slot++;
    auto slot = monitoring::detail::PairwisePreparedCaptureStorageAccess::take(
        *state_->capture_slots, capture_index);
    if (!slot)
        std::terminate();
    state_->active_slot.emplace(std::move(*slot));
    monitoring::detail::PairwisePreparedCaptureStorageAccess::set_channel_state(
        *state_->active_slot, monitoring::MonitoringChannel::page_faults,
        monitoring::MonitoringSignalState::not_applicable);
    monitoring::detail::PairwisePreparedCaptureStorageAccess::set_channel_state(
        *state_->active_slot, monitoring::MonitoringChannel::context_switches,
        monitoring::MonitoringSignalState::not_applicable);
    monitoring::detail::PairwisePreparedCaptureStorageAccess::set_channel_state(
        *state_->active_slot, monitoring::MonitoringChannel::temperature,
        state_->temperature_unavailable
            ? monitoring::MonitoringSignalState::unavailable
            : monitoring::MonitoringSignalState::not_applicable);
    monitoring::detail::PairwisePreparedCaptureStorageAccess::set_channel_state(
        *state_->active_slot, monitoring::MonitoringChannel::frequency,
        state_->frequency_unavailable
            ? monitoring::MonitoringSignalState::unavailable
            : monitoring::MonitoringSignalState::not_applicable);
    ++state_->generation;
    state_->active_registration = capture_index;
    if (!state_->active_registration_ptr()->activate(state_->generation))
        std::terminate();
    state_->capture_active = true;
    if (state_->capacity.page_requested()) {
        auto read = read_page_round(*state_, *state_->active_slot, platform,
                                    true, capture_index);
        if (!read) {
            if (!state_->active_registration_ptr()->unregister(state_->generation))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{std::move(read.error())};
        }
    }
    if (state_->async) {
        auto& async = *state_->async;
        std::unique_lock lock{async.mutex};
        async.generation = state_->generation;
        async.terminal_failure.reset();
        async.arm_requested = true;
        async.finish_requested = false;
        async.abort_requested = false;
        async.armed = false;
        async.quiesced = false;
        async.publication_enabled = true;
        async.round_in_flight = false;
        async.next_ordinal = 0U;
        async.condition.notify_all();
        async.condition.wait(lock, [&] {
            return async.armed || async.quiesced;
        });
        if (async.terminal_failure) {
            auto failure = std::move(*async.terminal_failure);
            async.terminal_failure.reset();
            lock.unlock();
            if (!state_->active_registration_ptr()->unregister(state_->generation))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{std::move(failure)};
        }
    }
    return PairwiseMonitoringActiveCapture{
        state_.get(), state_->generation, state_->capture_registrations,
        *state_->active_registration};
}

auto PairwiseMonitoringOwningFacade::complete_capture(
    PairwiseMonitoringActiveCapture capture,
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end,
    const PairwisePlatformLeafFacade& platform) noexcept
    -> std::expected<PairwiseMonitoringTerminalCapture,
                     PairwiseMonitoringCaptureFailure> {
    const auto valid = capture.registration() &&
        capture.registration()->active(capture.generation_) &&
        capture.state_ == state_.get() &&
        state_ && state_->capture_active && state_->active_slot &&
        capture.generation_ == state_->generation;
    if (!valid)
        return std::unexpected{monitoring_driver_transition_failure(
            PairwiseFailureCode::monitoring_invalid_state,
            PairwiseFailureReason::complete,
            PairwiseLifecyclePhase::complete_capture)};
    if (state_->async) {
        auto& async = *state_->async;
        std::unique_lock lock{async.mutex};
        if (async.terminal_failure) {
            async.condition.wait(lock, [&] { return async.quiesced; });
            auto failure = std::move(*async.terminal_failure);
            async.terminal_failure.reset();
            lock.unlock();
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{std::move(failure)};
        }
    }
    if (observation_end < observation_start) {
        std::optional<PairwiseMonitoringResetFailureSequence> reset_failures;
        if (state_->async) {
            auto& async = *state_->async;
            std::unique_lock lock{async.mutex};
            async.publication_enabled = false;
            async.abort_requested = true;
            async.condition.notify_all();
            monitoring::detail::wake_raw_monotonic(
                async.scheduler_mutex, async.scheduler_condition,
                async.scheduler_wake);
            async.condition.wait(lock, [&] { return async.quiesced; });
            async.terminal_failure.reset();
            if (async.reset_failures) {
                reset_failures.emplace(std::move(*async.reset_failures));
                async.reset_failures.reset();
            }
        }
        if (!capture.registration()->unregister(capture.generation_))
            std::terminate();
        state_->active_registration.reset();
        state_->capture_active = false;
        state_->active_slot.reset();
        auto failure = PairwiseMonitoringCaptureFailure{
            monitoring_page_classifier_failure(
                monitoring::detail::PageFaultClassifierFailure::
                    reversed_observation_interval)};
        if (reset_failures)
            return std::unexpected{append_reset_cleanup(
                std::move(failure), std::move(*reset_failures))};
        return std::unexpected{std::move(failure)};
    }
    const auto capture_index = state_->next_capture_slot - 1U;
    if (state_->capacity.page_requested()) {
        auto read = read_page_round(*state_, *state_->active_slot, platform,
                                    false, capture_index);
        if (!read) {
            std::optional<PairwiseMonitoringResetFailureSequence> reset_failures;
            if (state_->async) {
                auto& async = *state_->async;
                std::unique_lock lock{async.mutex};
                async.publication_enabled = false;
                async.abort_requested = true;
                async.condition.notify_all();
                monitoring::detail::wake_raw_monotonic(
                    async.scheduler_mutex, async.scheduler_condition,
                    async.scheduler_wake);
                async.condition.wait(lock, [&] { return async.quiesced; });
                async.terminal_failure.reset();
                if (async.reset_failures) {
                    reset_failures.emplace(std::move(*async.reset_failures));
                    async.reset_failures.reset();
                }
            }
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            auto failure = std::move(read.error());
            if (reset_failures)
                return std::unexpected{append_reset_cleanup(
                    std::move(failure), std::move(*reset_failures))};
            return std::unexpected{std::move(failure)};
        }
    }
    if (state_->async) {
        auto& async = *state_->async;
        std::unique_lock lock{async.mutex};
        async.publication_enabled = false;
        async.finish_requested = true;
        async.condition.notify_all();
        monitoring::detail::wake_raw_monotonic(
            async.scheduler_mutex, async.scheduler_condition,
            async.scheduler_wake);
        async.condition.wait(lock, [&] { return async.quiesced; });
        if (async.terminal_failure) {
            auto failure = std::move(*async.terminal_failure);
            async.terminal_failure.reset();
            lock.unlock();
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{std::move(failure)};
        }
    }
    if (state_->capacity.page_requested()) {
        const auto pairs = monitoring::detail::
            PairwisePreparedCaptureStorageAccess::prepare_page_pairs(
                *state_->classifier_scratch, state_->workers,
                monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    page_before(*state_->active_slot),
                monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    page_after(*state_->active_slot));
        if (!pairs)
            std::terminate();
        const monitoring::detail::PageFaultEvidenceSink sink{
            &*state_->active_slot,
            +[](void* raw, core::WorkerId worker,
                monitoring::MonitoringSignalState signal,
                std::optional<monitoring::PageFaultCount> minor,
                std::optional<monitoring::PageFaultCount> major) noexcept {
                return monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    append_page_evidence(
                        *static_cast<monitoring::detail::
                            PairwisePreparedCaptureSlot*>(raw),
                        worker, signal, minor, major);
            }};
        auto classified = monitoring::detail::classify_page_faults_bounded(
            observation_start, observation_end, *pairs, sink);
        if (!classified) {
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{PairwiseMonitoringCaptureFailure{
                monitoring_page_classifier_failure(classified.error())}};
        }
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                finalize_page_evidence(*state_->active_slot, *classified))
            std::terminate();
    }
    if (state_->capacity.context_requested()) {
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                finalize_context_acquisitions(
                    *state_->active_slot, state_->workers.size(),
                    static_cast<std::size_t>(state_->capacity.round_capacity())) ||
            !monitoring::detail::PairwisePreparedCaptureStorageAccess::
                prepare_context_views(
                    *state_->classifier_scratch, *state_->active_slot,
                    state_->workers, state_->context_coverage_start,
                    state_->context_coverage_end, state_->context_completeness))
            std::terminate();
        const monitoring::detail::ContextSwitchEvidenceSink sink{
            &*state_->active_slot,
            +[](void* raw, core::WorkerId worker,
                monitoring::MonitoringSignalState signal,
                std::optional<monitoring::ContextSwitchCount> voluntary,
                std::optional<monitoring::ContextSwitchCount> involuntary) noexcept {
                return monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    append_context_evidence(
                        *static_cast<monitoring::detail::
                            PairwisePreparedCaptureSlot*>(raw),
                        worker, signal, voluntary, involuntary);
            }};
        auto classified = monitoring::detail::classify_context_switches_bounded(
            observation_start, observation_end,
            monitoring::detail::PairwisePreparedCaptureStorageAccess::
                context_workers(*state_->classifier_scratch),
            monitoring::detail::PairwisePreparedCaptureStorageAccess::
                context_views(*state_->classifier_scratch),
            sink);
        if (!classified) {
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{PairwiseMonitoringCaptureFailure{
                monitoring_context_classifier_failure(classified.error())}};
        }
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                finalize_context_evidence(*state_->active_slot, *classified))
            std::terminate();
    }
    enum class PointSelectionFailure : unsigned char { ordering };
    const auto inside_points = [&](auto acquisitions)
        -> std::expected<decltype(acquisitions), PointSelectionFailure> {
        std::size_t first{}, count{};
        std::optional<platform_linux::RawMonotonicTimestamp> previous;
        for (std::size_t index{}; index < acquisitions.size(); ++index) {
            const auto& item = acquisitions[index];
            if (previous && item.collection_start() < *previous)
                return std::unexpected{PointSelectionFailure::ordering};
            previous = item.collection_start();
            // Raw facts remain retained. Only wholly contained acquisitions
            // enter the compact view, and its subspan must have no holes.
            if (item.collection_start() >= observation_start &&
                item.collection_end() <= observation_end) {
                if (count == 0U) first = index;
                else if (index - first != count)
                    return std::unexpected{PointSelectionFailure::ordering};
                ++count;
            }
        }
        return acquisitions.subspan(first, count);
    };
    if (state_->temperature_supported) {
        const auto interval =
            state_->capacity.admission().interval_nanoseconds();
        if (!interval)
            std::terminate();
        auto selected = inside_points(monitoring::detail::PairwisePreparedCaptureStorageAccess::
            raw_temperature(*state_->active_slot));
        auto classified = [&]() -> std::expected<monitoring::detail::TemperatureClassification,
                                               monitoring::detail::TemperatureClassifierFailure> {
            if (!selected) return std::unexpected{
                monitoring::detail::TemperatureClassifierFailure::ordering};
            return monitoring::detail::classify_temperature_bounded(
            observation_start, observation_end,
            core::Duration::from_base_units(*interval),
            monitoring::detail::PairwisePreparedCaptureStorageAccess::
                        missed_rounds(*state_->active_slot) == 0U
                ? monitoring::TemperatureCaptureCompleteness::complete
                : monitoring::TemperatureCaptureCompleteness::incomplete,
            *selected,
            monitoring::detail::PairwisePreparedCaptureStorageAccess::
                temperature_view_scratch(*state_->classifier_scratch));
        }();
        if (!classified) {
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{PairwiseMonitoringCaptureFailure{
                monitoring_temperature_classifier_failure(
                    classified.error())}};
        }
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                finalize_temperature_evidence(
                    *state_->active_slot, *classified,
                    core::Duration::from_base_units(*interval),
                    monitoring::TemperatureCaptureCompleteness::complete))
            std::terminate();
    }
    if (state_->frequency_supported) {
        const auto interval =
            state_->capacity.admission().interval_nanoseconds();
        if (!interval || state_->frequency_domain_count == 0U)
            std::terminate();
        for (std::size_t domain{};
             domain < state_->frequency_domain_count; ++domain) {
            const auto& metadata = state_->frequency_metadata[domain];
            if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                    prepare_frequency_public_domain(
                        *state_->active_slot, domain, metadata.cpus,
                        metadata.kind, metadata.driver, metadata.governor,
                        metadata.minimum_hz, metadata.maximum_hz))
                std::terminate();
        }
        struct FrequencySinkContext final {
            monitoring::detail::PairwisePreparedCaptureSlot* slot{};
            const std::vector<PairwisePreparedFrequencyDomain>* metadata{};
            std::size_t count{};
            std::size_t domain{};
        } sink_context{&*state_->active_slot, &state_->frequency_metadata,
                       state_->frequency_domain_count, 0U};
        const monitoring::detail::FrequencyEvidenceSink sink{
            &sink_context,
            +[](void* raw,
                const monitoring::detail::FrequencyDomainClassification&
                    value) noexcept {
                auto& context = *static_cast<FrequencySinkContext*>(raw);
                if (context.domain >= context.count)
                    return false;
                const auto domain = context.domain++;
                return monitoring::detail::
                    PairwisePreparedCaptureStorageAccess::
                        append_frequency_classification(
                            *context.slot, domain, value,
                            (*context.metadata)[domain].cpus);
            }};
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::prepare_frequency_classifier_domains(
                *state_->classifier_scratch,
                monitoring::detail::PairwisePreparedCaptureStorageAccess::raw_frequency(*state_->active_slot)))
            std::terminate();
        auto domains = monitoring::detail::PairwisePreparedCaptureStorageAccess::
            frequency_classifier_domains(*state_->classifier_scratch);
        auto classified = [&]() -> std::expected<monitoring::MonitoringSignalState,
                                               monitoring::detail::FrequencyClassifierFailure> {
            for (auto& domain : domains) {
                auto selected = inside_points(domain.acquisitions);
                if (!selected) return std::unexpected{
                    monitoring::detail::FrequencyClassifierFailure::ordering};
                domain.acquisitions = *selected;
            }
            // Every authenticated domain remains, including an empty view;
            // the unchanged kernel decides insufficient/unavailable evidence.
            return monitoring::detail::classify_frequency_bounded(
            observation_start, observation_end,
            core::Duration::from_base_units(*interval),
            monitoring::detail::PairwisePreparedCaptureStorageAccess::
                        missed_rounds(*state_->active_slot) == 0U
                ? monitoring::FrequencyCaptureCompleteness::complete
                : monitoring::FrequencyCaptureCompleteness::incomplete,
            domains,
            monitoring::detail::PairwisePreparedCaptureStorageAccess::
                frequency_view_scratch(*state_->classifier_scratch),
            monitoring::detail::PairwisePreparedCaptureStorageAccess::
                frequency_result_scratch(*state_->classifier_scratch),
            sink);
        }();
        if (!classified) {
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{PairwiseMonitoringCaptureFailure{
                monitoring_frequency_classifier_failure(
                    classified.error())}};
        }
        if (!monitoring::detail::PairwisePreparedCaptureStorageAccess::
                finalize_frequency_evidence(
                    *state_->active_slot, *classified,
                    core::Duration::from_base_units(*interval),
                    monitoring::FrequencyCaptureCompleteness::complete))
            std::terminate();
    }
    if (!capture.registration()->unregister(capture.generation_))
        std::terminate();
    state_->active_registration.reset();
    state_->capture_active = false;
    auto terminal = PairwiseMonitoringTerminalCapture{
        std::move(*state_->active_slot)};
    state_->active_slot.reset();
    return terminal;
}

auto PairwiseMonitoringOwningFacade::abort_capture(
    PairwiseMonitoringActiveCapture capture) noexcept
    -> std::expected<void, PairwiseMonitoringCaptureFailure> {
    const auto valid = capture.registration() &&
        capture.registration()->active(capture.generation_) &&
        capture.state_ == state_.get() &&
        state_ && state_->capture_active && state_->active_slot &&
        capture.generation_ == state_->generation;
    if (!valid)
        return std::unexpected{monitoring_driver_transition_failure(
            PairwiseFailureCode::monitoring_invalid_state,
            PairwiseFailureReason::abort,
            PairwiseLifecyclePhase::abort_capture)};
    if (state_->async) {
        auto& async = *state_->async;
        std::unique_lock lock{async.mutex};
        async.publication_enabled = false;
        async.abort_requested = true;
        async.condition.notify_all();
        monitoring::detail::wake_raw_monotonic(
            async.scheduler_mutex, async.scheduler_condition,
            async.scheduler_wake);
        async.condition.wait(lock, [&] { return async.quiesced; });
        if (async.terminal_failure) {
            auto failure = std::move(*async.terminal_failure);
            async.terminal_failure.reset();
            lock.unlock();
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{std::move(failure)};
        }
        if (async.reset_failures) {
            auto failure = reset_sequence_primary(
                std::move(*async.reset_failures));
            async.reset_failures.reset();
            lock.unlock();
            if (!capture.registration()->unregister(capture.generation_))
                std::terminate();
            state_->active_registration.reset();
            state_->capture_active = false;
            state_->active_slot.reset();
            return std::unexpected{std::move(failure)};
        }
    }
    if (!capture.registration()->unregister(capture.generation_))
        std::terminate();
    state_->active_registration.reset();
    state_->capture_active = false;
    state_->active_slot.reset();
    return {};
}

auto PairwiseMonitoringOwningFacade::progress_quiescent(bool retired) const noexcept -> bool {
    if (!state_ || state_->capture_active || state_->active_registration || state_->active_slot)
        return false;
    if (retired && !state_->stopped) return false;
    for (std::size_t i = 0; i < state_->capture_registration_count; ++i)
        if (state_->capture_registrations.get()[i].state() == PairwiseMonitoringCaptureRegistrationState::active)
            return false;
    if (state_->async) {
        auto& async = *state_->async;
        std::lock_guard lock{async.mutex};
        if (!async.quiesced || async.publication_enabled || async.round_in_flight) return false;
        if (retired && async.service && async.service->joinable()) return false;
    }
    return true;
}

auto PairwiseMonitoringOwningFacade::stop_and_retire(
    const PairwisePlatformLeafFacade& platform) noexcept
    -> PairwiseMonitoringStopOutcome {
    bool invariant{};
    std::size_t failure_count{};
    std::size_t frequency_source_retirement_count{};
    const auto append_operation = [&](
        PairwiseMonitoringOperationFailure failure) noexcept {
        if (failure_count >= state_->stop_returned.size()) {
            invariant = true;
            return;
        }
        state_->stop_returned[failure_count++].emplace(std::move(failure));
    };
    const auto append = [&](PairwisePlatformFailure failure) noexcept {
        auto mapped = PairwiseMonitoringOperationFailure::from_platform(
            std::move(failure));
        if (!mapped) {
            invariant = true;
            return;
        }
        append_operation(std::move(*mapped));
    };
    if (state_->capture_active) {
        append_operation(monitoring_active_capture_stop_failure());
        if (state_->async) {
            auto& async = *state_->async;
            std::unique_lock lock{async.mutex};
            async.publication_enabled = false;
            async.abort_requested = true;
            async.condition.notify_all();
            monitoring::detail::wake_raw_monotonic(
                async.scheduler_mutex, async.scheduler_condition,
                async.scheduler_wake);
            async.condition.wait(lock, [&] { return async.quiesced; });
            async.terminal_failure.reset();
            if (async.reset_failures) {
                for (std::size_t index{};
                     index < async.reset_failures->count; ++index)
                    if (async.reset_failures->values[index])
                        append_operation(std::move(
                            *async.reset_failures->values[index]));
                async.reset_failures.reset();
            }
        }
        if (!state_->active_registration || !state_->active_slot)
            invariant = true;
        else
            invariant = !state_->active_registration_ptr()->retire(state_->generation) ||
                invariant;
        state_->active_registration.reset();
        state_->active_slot.reset();
        state_->capture_active = false;
    }
    if (state_->async && state_->async->service) {
        auto& async = *state_->async;
        {
            std::lock_guard lock{async.mutex};
            async.lifetime_stop = true;
            async.publication_enabled = false;
        }
        async.condition.notify_all();
        monitoring::detail::wake_raw_monotonic(
            async.scheduler_mutex, async.scheduler_condition,
            async.scheduler_wake);
        async.stop.request_stop();
        async.service->request_stop();
        async.wake.store(true, std::memory_order_release);
        async.wake.notify_all();
        const auto ordinary = terminalize_monitoring_service(*state_);
        if (ordinary == PairwiseMonitoringServiceJoinFault::native_failure ||
            ordinary == PairwiseMonitoringServiceJoinFault::contained_standard) {
            append_operation(monitoring_stop_contained(
                PairwiseFailureReason::contained_exception));
        } else if (ordinary ==
                   PairwiseMonitoringServiceJoinFault::contained_unknown) {
            append_operation(monitoring_stop_contained(
                PairwiseFailureReason::contained_unknown_failure));
        }
    }
    for (auto source = state_->context_sources.rbegin();
         source != state_->context_sources.rend(); ++source) {
        auto retired = platform.retire_perf_context_switch(std::move(*source));
        if (retired.primary)
            append(std::move(*retired.primary));
        if (retired.cleanup)
            append(std::move(*retired.cleanup));
    }
    state_->context_sources.clear();
    const auto remaining = [&]() noexcept
        -> std::span<platform_linux::detail::BoundedSysfsDisposition> {
        if (state_->source_retirement_count >= state_->source_retirement.size())
            return {};
        return std::span{state_->source_retirement}.subspan(
            state_->source_retirement_count);
    };
    if (state_->frequency_sources) {
        const auto available = remaining().size();
        const auto retired = platform.retire_cpu_frequencies(
            std::move(*state_->frequency_sources), remaining());
        state_->frequency_sources.reset();
        if (retired > available) {
            invariant = true;
            state_->source_retirement_count = state_->source_retirement.size();
        } else {
            state_->source_retirement_count += retired;
        }
        frequency_source_retirement_count =
            state_->source_retirement_count;
    }
    if (state_->temperature_source) {
        const auto available = remaining().size();
        const auto retired = platform.retire_hwmon_temperature(
            std::move(*state_->temperature_source), remaining());
        state_->temperature_source.reset();
        if (retired > available) {
            invariant = true;
            state_->source_retirement_count = state_->source_retirement.size();
        } else {
            state_->source_retirement_count += retired;
        }
    }
    state_->retirement_platform.reset();
    state_->stopped = true;
    return PairwiseMonitoringStopOutcome{
        std::move(state_->stop_returned), failure_count,
        std::move(state_->source_retirement), state_->source_retirement_count,
        frequency_source_retirement_count,
        state_->async ? state_->async->force
                      : std::array<PairwiseMonitoringForceDisposition, 3>{},
        state_->async ? state_->async->force_count : 0U, invariant};
}
auto PairwiseMonitoringOwningFacade::context_offset_slot_count() const noexcept
    -> std::size_t {
    return monitoring::detail::PairwisePreparedCaptureStorageAccess::
        context_offset_capacity(*state_->capture_slots);
}
auto PairwiseMonitoringOwningFacade::context_slot_count() const noexcept
    -> std::size_t {
    return monitoring::detail::PairwisePreparedCaptureStorageAccess::
        context_capacity(*state_->capture_slots);
}
auto PairwiseMonitoringOwningFacade::context_event_slot_count() const noexcept
    -> std::size_t {
    return monitoring::detail::PairwisePreparedCaptureStorageAccess::
        context_event_capacity(*state_->capture_slots);
}
auto PairwiseMonitoringOwningFacade::frequency_nested_cpu_slot_count() const noexcept
    -> std::size_t {
    return monitoring::detail::PairwisePreparedCaptureStorageAccess::
        frequency_nested_cpu_capacity(*state_->capture_slots);
}
auto PairwiseMonitoringOwningFacade::frequency_supported() const noexcept -> bool {
    return state_->frequency_supported;
}
auto PairwiseMonitoringOwningFacade::persistent_frequency_cpu_capacity() const noexcept
    -> std::size_t {
    return state_->frequency_cpu_storage.capacity() * 2U;
}
auto PairwiseMonitoringOwningFacade::rollback_returned_capacity() const noexcept
    -> std::size_t { return state_->rollback_returned.size(); }
auto PairwiseMonitoringOwningFacade::completion_returned_capacity() const noexcept
    -> std::size_t { return state_->completion_returned.size(); }
auto PairwiseMonitoringOwningFacade::stop_returned_capacity() const noexcept
    -> std::size_t { return state_->stop_returned.size(); }
auto PairwiseMonitoringOwningFacade::page_read_disposition_capacity() const noexcept
    -> std::size_t { return state_->page_read_dispositions.size(); }
auto PairwiseMonitoringOwningFacade::force_disposition_capacity() const noexcept
    -> std::size_t { return state_->async ? state_->async->force.size() : 0U; }
auto PairwiseMonitoringOwningFacade::force_disposition_count() const noexcept
    -> std::size_t { return state_->async ? state_->async->force_count : 0U; }
auto PairwiseMonitoringOwningFacade::has_service_owner() const noexcept -> bool {
    return state_->service_owner;
}

auto PairwiseMonitoringPreparationTestAccess::set_service_join_script(
    PairwiseMonitoringOwningFacade& facade,
    std::span<const PairwiseMonitoringServiceJoinFault> outcomes) noexcept
    -> bool {
    if (!facade.state_ || !facade.state_->async ||
        outcomes.size() > facade.state_->async->join_script.size())
        return false;
    auto& async = *facade.state_->async;
    std::ranges::copy(outcomes, async.join_script.begin());
    async.join_script_count = outcomes.size();
    async.join_script_position = 0U;
    return true;
}

namespace {

[[nodiscard]] auto monitoring_storage_identity(
    const PairwiseMonitoringOwningFacade::State* state) noexcept
    -> PairwiseMonitoringPreparationTestAccess::StorageIdentity {
    if (state == nullptr)
        return {};
    return {
        .state = state,
        .workers = state->workers.data(),
        .worker_capacity = state->workers.capacity(),
        .capture_slots = state->capture_slots ? &*state->capture_slots : nullptr,
        .classifier_scratch = state->classifier_scratch
            ? &*state->classifier_scratch
            : nullptr,
        .registrations = state->capture_registrations.get(),
        .registration_count = state->capture_registration_count,
        .frequency_cpus = state->frequency_cpu_storage.data(),
        .frequency_cpu_capacity = state->frequency_cpu_storage.capacity(),
        .frequency_metadata_count = state->frequency_domain_count,
        .frequency_partition_committed = state->frequency_partition_committed,
        .frequency_layout = state->capture_slots && state->classifier_scratch
            ? monitoring::detail::PairwisePreparedCaptureStorageAccess::
                  frequency_layout(*state->capture_slots,
                                   *state->classifier_scratch)
            : monitoring::detail::PairwisePreparedFrequencyLayoutObservation{},
    };
}

} // namespace

auto PairwiseMonitoringPreparationTestAccess::storage_identity(
    const PairwisePreparedMonitoringStorage& prepared) noexcept
    -> StorageIdentity {
    auto result = monitoring_storage_identity(
        prepared.state_ ? prepared.state_->storage.get() : nullptr);
    if (prepared.state_) {
        result.rollback_cleanup = prepared.state_->rollback_cleanup.data();
        result.rollback_cleanup_capacity =
            prepared.state_->rollback_cleanup.capacity();
    }
    return result;
}

auto PairwiseMonitoringPreparationTestAccess::rollback_cleanup_capacity(
    bool temperature_requested, bool frequency_requested,
    std::size_t domains, std::size_t rollback_values) noexcept
    -> std::optional<std::size_t> {
    return monitoring_rollback_cleanup_capacity(
        temperature_requested, frequency_requested, domains, rollback_values);
}

auto PairwiseMonitoringPreparationTestAccess::storage_identity(
    const PairwiseBoundMonitoringPreparation& prepared) noexcept
    -> StorageIdentity {
    auto result = monitoring_storage_identity(
        prepared.state_ ? prepared.state_->storage.get() : nullptr);
    if (prepared.state_) {
        result.rollback_cleanup = prepared.state_->rollback_cleanup.data();
        result.rollback_cleanup_capacity =
            prepared.state_->rollback_cleanup.capacity();
    }
    return result;
}

auto PairwiseMonitoringPreparationTestAccess::storage_identity(
    const PairwiseMonitoringOwningFacade& facade) noexcept -> StorageIdentity {
    return monitoring_storage_identity(facade.state_.get());
}

auto PairwiseMonitoringPreparationTestAccess::set_service_run_fault(
    PairwiseMonitoringOwningFacade& facade,
    PairwiseMonitoringServiceRunFault outcome) noexcept -> bool {
    if (!facade.state_ || !facade.state_->async)
        return false;
    auto& async = *facade.state_->async;
    std::lock_guard lock{async.mutex};
    async.run_fault = outcome;
    return true;
}

auto PairwiseMonitoringPreparationTestAccess::active_activity(
    const PairwiseMonitoringOwningFacade& facade) noexcept
    -> std::optional<monitoring::MonitoringDriverActivityEvidence> {
    if (!facade.state_)
        return std::nullopt;
    std::unique_lock<std::mutex> lock;
    if (facade.state_->async) {
        auto& async = *facade.state_->async;
        lock = std::unique_lock{async.mutex};
        // Source rounds and final drain/reset update outside this mutex.
        // Copy only after quiescence or while active scheduling is idle.
        if (!async.quiesced &&
            (!async.armed || !async.publication_enabled ||
             async.round_in_flight || async.finish_requested ||
             async.abort_requested || async.lifetime_stop))
            return std::nullopt;
    }
    if (!facade.state_->active_slot)
        return std::nullopt;
    return monitoring::detail::PairwisePreparedCaptureStorageAccess::result(
               *facade.state_->active_slot)
        .activity();
}

auto PairwiseMonitoringPreparationTestAccess::active_first_temperature_start(
    const PairwiseMonitoringOwningFacade& facade) noexcept
    -> std::optional<platform_linux::RawMonotonicTimestamp> {
    if (!facade.state_ || !facade.state_->active_slot)
        return std::nullopt;
    const auto temperature =
        monitoring::detail::PairwisePreparedCaptureStorageAccess::result(
            *facade.state_->active_slot)
            .raw()
            .temperature();
    if (temperature.empty())
        return std::nullopt;
    return temperature.front().collection_start();
}

namespace {

struct ManifestProducer final {
    PairwiseMonitoringAuthorshipDisposition disposition;
    PairwiseMonitoringProducerFamily family;
    std::uint16_t branch;
};

[[nodiscard]] constexpr auto evidence_gate(
    PairwiseMonitoringProducerFamily producer) noexcept
    -> PairwiseMonitoringEvidenceGate {
    switch (producer) {
    case PairwiseMonitoringProducerFamily::platform_task_stat:
        return PairwiseMonitoringEvidenceGate::platform_leaf_task_stat_branches;
    case PairwiseMonitoringProducerFamily::platform_perf_context_switch:
        return PairwiseMonitoringEvidenceGate::platform_leaf_perf_branches;
    case PairwiseMonitoringProducerFamily::platform_hwmon_temperature:
        return PairwiseMonitoringEvidenceGate::platform_leaf_hwmon_branches;
    case PairwiseMonitoringProducerFamily::platform_cpu_frequency:
        return PairwiseMonitoringEvidenceGate::platform_leaf_cpu_frequency_branches;
    case PairwiseMonitoringProducerFamily::platform_service_placement:
        return PairwiseMonitoringEvidenceGate::platform_leaf_service_placement_branches;
    case PairwiseMonitoringProducerFamily::platform_raw_clock:
        return PairwiseMonitoringEvidenceGate::platform_leaf_raw_clock_branches;
    case PairwiseMonitoringProducerFamily::admitted_policy_precondition:
        return PairwiseMonitoringEvidenceGate::production_admission_policy_precondition;
    case PairwiseMonitoringProducerFamily::prepared_operations_precondition:
        return PairwiseMonitoringEvidenceGate::source_transaction_operations_precondition;
    case PairwiseMonitoringProducerFamily::typed_platform_precondition:
        return PairwiseMonitoringEvidenceGate::platform_capability_precondition;
    case PairwiseMonitoringProducerFamily::noexcept_driver_precondition:
        return PairwiseMonitoringEvidenceGate::noexcept_lifecycle_precondition;
    default:
        return PairwiseMonitoringEvidenceGate::owning_facade_direct_branch;
    }
}

[[nodiscard]] constexpr auto raw_clock_code(PairwiseFailureCode code) noexcept
    -> bool {
    return code == PairwiseFailureCode::platform_raw_clock_read_failed ||
           code == PairwiseFailureCode::platform_raw_clock_conversion_failed ||
           code == PairwiseFailureCode::platform_raw_clock_invalid_timestamp ||
           code == PairwiseFailureCode::platform_raw_clock_contained_exception ||
           code == PairwiseFailureCode::platform_raw_clock_contained_unknown_failure;
}

[[nodiscard]] constexpr auto platform_family(PairwiseFailureCode code) noexcept
    -> PairwiseMonitoringProducerFamily {
    switch (code) {
    case PairwiseFailureCode::platform_task_stat_read_failed:
    case PairwiseFailureCode::platform_task_stat_malformed:
    case PairwiseFailureCode::platform_task_stat_counter_out_of_range:
    case PairwiseFailureCode::platform_task_stat_identity_mismatch:
        return PairwiseMonitoringProducerFamily::platform_task_stat;
    case PairwiseFailureCode::platform_perf_context_switch_invalid_configuration:
    case PairwiseFailureCode::platform_perf_context_switch_open_failed:
    case PairwiseFailureCode::platform_perf_context_switch_map_failed:
    case PairwiseFailureCode::platform_perf_context_switch_construction_failed:
    case PairwiseFailureCode::platform_perf_context_switch_ring_corrupt:
    case PairwiseFailureCode::platform_perf_context_switch_record_malformed:
    case PairwiseFailureCode::platform_perf_context_switch_record_capacity:
    case PairwiseFailureCode::platform_perf_context_switch_record_unsupported:
    case PairwiseFailureCode::platform_perf_context_switch_invalid_state:
    case PairwiseFailureCode::platform_perf_context_switch_capture_incomplete:
    case PairwiseFailureCode::platform_perf_context_switch_previous_drain_failed:
    case PairwiseFailureCode::platform_perf_context_switch_epoch_control_failed:
    case PairwiseFailureCode::platform_perf_context_switch_epoch_read_failed:
    case PairwiseFailureCode::platform_perf_context_switch_epoch_invalid:
    case PairwiseFailureCode::platform_perf_context_switch_epoch_loss:
    case PairwiseFailureCode::platform_perf_context_switch_unmap_failed:
    case PairwiseFailureCode::platform_perf_context_switch_close_failed:
    case PairwiseFailureCode::platform_perf_context_switch_metadata_capacity_exceeded:
        return PairwiseMonitoringProducerFamily::platform_perf_context_switch;
    case PairwiseFailureCode::platform_hwmon_temperature_discovery_failed:
    case PairwiseFailureCode::platform_hwmon_temperature_read_failed:
    case PairwiseFailureCode::platform_hwmon_temperature_invalid_value:
    case PairwiseFailureCode::platform_hwmon_temperature_ambiguous_sensor:
    case PairwiseFailureCode::platform_hwmon_temperature_enumeration_capacity_exceeded:
    case PairwiseFailureCode::platform_hwmon_temperature_token_capacity_exceeded:
        return PairwiseMonitoringProducerFamily::platform_hwmon_temperature;
    default:
        return PairwiseMonitoringProducerFamily::platform_cpu_frequency;
    }
}

[[nodiscard]] constexpr auto classifier_branch(
    PairwiseFailureOperation operation, PairwiseFailureReason reason) noexcept
    -> std::uint16_t {
    using P = monitoring::detail::PageFaultClassifierFailure;
    using C = monitoring::detail::ContextSwitchClassifierFailure;
    using T = monitoring::detail::TemperatureClassifierFailure;
    using F = monitoring::detail::FrequencyClassifierFailure;
    switch (operation) {
    case PairwiseFailureOperation::classify_page_faults:
        switch (reason) {
        case PairwiseFailureReason::empty_workers: return static_cast<std::uint16_t>(P::empty_workers);
        case PairwiseFailureReason::duplicate_worker: return static_cast<std::uint16_t>(P::duplicate_worker);
        case PairwiseFailureReason::worker_scope: return static_cast<std::uint16_t>(P::worker_scope);
        case PairwiseFailureReason::acquisition_metadata: return static_cast<std::uint16_t>(P::acquisition_metadata);
        case PairwiseFailureReason::reversed_observation_interval: return static_cast<std::uint16_t>(P::reversed_observation_interval);
        case PairwiseFailureReason::before_overlaps_observation: return static_cast<std::uint16_t>(P::before_overlaps_observation);
        case PairwiseFailureReason::after_overlaps_observation: return static_cast<std::uint16_t>(P::after_overlaps_observation);
        default: return static_cast<std::uint16_t>(P::cumulative_counter_decreased);
        }
    case PairwiseFailureOperation::classify_context_switches:
        switch (reason) {
        case PairwiseFailureReason::empty_workers: return static_cast<std::uint16_t>(C::empty_workers);
        case PairwiseFailureReason::duplicate_worker: return static_cast<std::uint16_t>(C::duplicate_worker);
        case PairwiseFailureReason::reversed_observation_interval: return static_cast<std::uint16_t>(C::reversed_observation_interval);
        case PairwiseFailureReason::worker_view_count: return static_cast<std::uint16_t>(C::worker_view_count);
        case PairwiseFailureReason::worker_source: return static_cast<std::uint16_t>(C::worker_source);
        case PairwiseFailureReason::acquisition_metadata: return static_cast<std::uint16_t>(C::acquisition_metadata);
        case PairwiseFailureReason::batch_worker: return static_cast<std::uint16_t>(C::batch_worker);
        case PairwiseFailureReason::event_worker: return static_cast<std::uint16_t>(C::event_worker);
        case PairwiseFailureReason::interval_coverage: return static_cast<std::uint16_t>(C::interval_coverage);
        case PairwiseFailureReason::lost_records: return static_cast<std::uint16_t>(C::lost_records);
        default: return static_cast<std::uint16_t>(C::event_count);
        }
    case PairwiseFailureOperation::classify_temperature:
        switch (reason) {
        case PairwiseFailureReason::ordering: return static_cast<std::uint16_t>(T::ordering);
        case PairwiseFailureReason::interval: return static_cast<std::uint16_t>(T::interval);
        case PairwiseFailureReason::boundary_overlap: return static_cast<std::uint16_t>(T::boundary_overlap);
        case PairwiseFailureReason::metadata: return static_cast<std::uint16_t>(T::metadata);
        case PairwiseFailureReason::sensor: return static_cast<std::uint16_t>(T::sensor);
        case PairwiseFailureReason::driver: return static_cast<std::uint16_t>(T::driver);
        case PairwiseFailureReason::sample_count: return static_cast<std::uint16_t>(T::sample_count);
        case PairwiseFailureReason::leading_gap: return static_cast<std::uint16_t>(T::leading_gap);
        case PairwiseFailureReason::internal_gap: return static_cast<std::uint16_t>(T::internal_gap);
        default: return static_cast<std::uint16_t>(T::trailing_gap);
        }
    case PairwiseFailureOperation::classify_frequency:
        switch (reason) {
        case PairwiseFailureReason::ordering: return static_cast<std::uint16_t>(F::ordering);
        case PairwiseFailureReason::interval: return static_cast<std::uint16_t>(F::interval);
        case PairwiseFailureReason::boundary_overlap: return static_cast<std::uint16_t>(F::boundary_overlap);
        case PairwiseFailureReason::ratio: return static_cast<std::uint16_t>(F::ratio);
        case PairwiseFailureReason::domain_order: return static_cast<std::uint16_t>(F::domain_order);
        case PairwiseFailureReason::domain: return static_cast<std::uint16_t>(F::domain);
        case PairwiseFailureReason::metadata: return static_cast<std::uint16_t>(F::metadata);
        case PairwiseFailureReason::implementation: return static_cast<std::uint16_t>(F::implementation);
        case PairwiseFailureReason::source: return static_cast<std::uint16_t>(F::source);
        case PairwiseFailureReason::driver: return static_cast<std::uint16_t>(F::driver);
        case PairwiseFailureReason::sample_count: return static_cast<std::uint16_t>(F::sample_count);
        case PairwiseFailureReason::leading_gap: return static_cast<std::uint16_t>(F::leading_gap);
        case PairwiseFailureReason::internal_gap: return static_cast<std::uint16_t>(F::internal_gap);
        default: return static_cast<std::uint16_t>(F::trailing_gap);
        }
    default: return 0U;
    }
}

[[nodiscard]] constexpr auto manifest_producer(
    const PairwiseFinalizedErrorTuple& tuple) noexcept -> ManifestProducer {
    const auto subsystem = pairwise_failure_subsystem(tuple.code);
    if (subsystem == PairwiseFailureSubsystem::platform) {
        if (raw_clock_code(tuple.code))
            return {PairwiseMonitoringAuthorshipDisposition::delegated_clock_branch,
                    PairwiseMonitoringProducerFamily::platform_raw_clock,
                    static_cast<std::uint16_t>(tuple.reason)};
        if (service_placement_platform_relation(tuple.code, tuple.operation))
            return {PairwiseMonitoringAuthorshipDisposition::
                        delegated_service_placement_branch,
                    PairwiseMonitoringProducerFamily::platform_service_placement,
                    static_cast<std::uint16_t>(tuple.reason)};
        return {PairwiseMonitoringAuthorshipDisposition::delegated_platform_branch,
                platform_family(tuple.code),
                static_cast<std::uint16_t>(tuple.reason)};
    }
    if (tuple.operation == PairwiseFailureOperation::classify_page_faults)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::page_classifier,
                classifier_branch(tuple.operation, tuple.reason)};
    if (tuple.operation == PairwiseFailureOperation::classify_context_switches)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::context_classifier,
                classifier_branch(tuple.operation, tuple.reason)};
    if (tuple.operation == PairwiseFailureOperation::classify_temperature)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::temperature_classifier,
                classifier_branch(tuple.operation, tuple.reason)};
    if (tuple.operation == PairwiseFailureOperation::classify_frequency)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::frequency_classifier,
                classifier_branch(tuple.operation, tuple.reason)};
    if (tuple.code == PairwiseFailureCode::monitoring_required_source_unsupported)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::required_source,
                static_cast<std::uint16_t>(tuple.context)};
    if (tuple.code == PairwiseFailureCode::monitoring_invalid_state ||
        (tuple.code == PairwiseFailureCode::monitoring_driver_invariant_failed &&
         tuple.reason == PairwiseFailureReason::generation_exhausted))
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::driver_transition,
                static_cast<std::uint16_t>(tuple.reason)};
    if (tuple.code == PairwiseFailureCode::monitoring_scheduled_capacity_exceeded)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::scheduled_capacity, 0U};
    if (tuple.code == PairwiseFailureCode::monitoring_driver_invariant_failed &&
        tuple.reason == PairwiseFailureReason::deadline_overflow)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::driver_transition,
                static_cast<std::uint16_t>(tuple.reason)};
    if (tuple.code == PairwiseFailureCode::monitoring_linux_provider_contained_failure &&
        tuple.context == PairwiseContextConstraint::phase_prepare)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::provider_prepare_containment,
                static_cast<std::uint16_t>(tuple.reason)};
    if ((tuple.code == PairwiseFailureCode::monitoring_driver_contained_exception ||
         tuple.code == PairwiseFailureCode::monitoring_driver_contained_unknown_failure) &&
        tuple.context == PairwiseContextConstraint::phase_service_exception)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::service_containment,
                static_cast<std::uint16_t>(tuple.reason)};
    if ((tuple.code == PairwiseFailureCode::monitoring_driver_contained_exception ||
         tuple.code == PairwiseFailureCode::monitoring_driver_contained_unknown_failure) &&
        tuple.context == PairwiseContextConstraint::phase_stop_and_join)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::stop_containment,
                static_cast<std::uint16_t>(tuple.reason)};
    if (tuple.code == PairwiseFailureCode::monitoring_service_start_failed)
        return {PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch,
                PairwiseMonitoringProducerFamily::service_placement,
                static_cast<std::uint16_t>(tuple.reason)};
    if (tuple.code == PairwiseFailureCode::monitoring_invalid_policy)
        return {PairwiseMonitoringAuthorshipDisposition::precluded_by_authenticated_precondition,
                PairwiseMonitoringProducerFamily::admitted_policy_precondition,
                static_cast<std::uint16_t>(tuple.reason)};
    if (tuple.code == PairwiseFailureCode::monitoring_missing_operation ||
        tuple.code == PairwiseFailureCode::monitoring_linux_provider_invalid_state)
        return {PairwiseMonitoringAuthorshipDisposition::precluded_by_authenticated_precondition,
                PairwiseMonitoringProducerFamily::prepared_operations_precondition,
                static_cast<std::uint16_t>(tuple.reason)};
    if (tuple.code == PairwiseFailureCode::monitoring_linux_provider_contained_failure)
        return {PairwiseMonitoringAuthorshipDisposition::precluded_by_authenticated_precondition,
                PairwiseMonitoringProducerFamily::typed_platform_precondition,
                static_cast<std::uint16_t>(tuple.reason)};
    return {PairwiseMonitoringAuthorshipDisposition::precluded_by_authenticated_precondition,
            PairwiseMonitoringProducerFamily::noexcept_driver_precondition,
            static_cast<std::uint16_t>(tuple.reason)};
}

[[nodiscard]] auto authored_tuple(
    const PairwiseMonitoringOperationFailure& value) noexcept
    -> PairwiseMonitoringAuthoredTuple {
    return {value.code(), value.operation(), value.reason(), value.context(),
            value.posix_errno()};
}

} // namespace

auto pairwise_monitoring_authorship_manifest() noexcept
    -> std::span<const PairwiseMonitoringAuthorshipEntry> {
    static const auto entries = [] {
        std::vector<PairwiseMonitoringAuthorshipEntry> result;
        result.reserve(154U);
        for (const auto& tuple : pairwise_finalized_error_tuples()) {
            const auto subsystem = pairwise_failure_subsystem(tuple.code);
            if (subsystem != PairwiseFailureSubsystem::monitoring &&
                !(subsystem == PairwiseFailureSubsystem::platform &&
                  (monitoring_platform_code(tuple.code) ||
                   service_placement_platform_relation(tuple.code,
                                                       tuple.operation) ||
                   raw_clock_code(tuple.code))))
                continue;
            const auto producer = manifest_producer(tuple);
            result.push_back({tuple,
                              pairwise_context_for_constraint(tuple.context,
                                                              tuple.reason),
                              tuple.native_rule == PairwiseNativeRule::required_posix_errno
                                  ? std::optional<int>{1}
                                  : std::nullopt,
                              producer.disposition, producer.family,
                              evidence_gate(producer.family),
                              producer.branch});
        }
        return result;
    }();
    return entries;
}

auto PairwiseMonitoringAuthorshipTestAccess::invoke_direct(
    const PairwiseMonitoringAuthorshipEntry& entry) noexcept
    -> std::optional<PairwiseMonitoringAuthoredTuple> {
    if (entry.disposition !=
        PairwiseMonitoringAuthorshipDisposition::direct_monitoring_branch)
        return std::nullopt;
    PairwiseMonitoringOperationFailure value = [&] {
        switch (entry.producer) {
        case PairwiseMonitoringProducerFamily::required_source: {
            const auto channel = entry.tuple.context == PairwiseContextConstraint::channel_page_faults
                ? monitoring::MonitoringChannel::page_faults
                : entry.tuple.context == PairwiseContextConstraint::channel_context_switches
                ? monitoring::MonitoringChannel::context_switches
                : entry.tuple.context == PairwiseContextConstraint::channel_temperature
                ? monitoring::MonitoringChannel::temperature
                : monitoring::MonitoringChannel::frequency;
            return monitoring_required_source_unsupported(channel);
        }
        case PairwiseMonitoringProducerFamily::page_classifier:
            return monitoring_page_classifier_failure(
                static_cast<monitoring::detail::PageFaultClassifierFailure>(entry.branch));
        case PairwiseMonitoringProducerFamily::context_classifier:
            return monitoring_context_classifier_failure(
                static_cast<monitoring::detail::ContextSwitchClassifierFailure>(entry.branch));
        case PairwiseMonitoringProducerFamily::temperature_classifier:
            return monitoring_temperature_classifier_failure(
                static_cast<monitoring::detail::TemperatureClassifierFailure>(entry.branch));
        case PairwiseMonitoringProducerFamily::frequency_classifier:
            return monitoring_frequency_classifier_failure(
                static_cast<monitoring::detail::FrequencyClassifierFailure>(entry.branch));
        case PairwiseMonitoringProducerFamily::provider_prepare_containment:
            return monitoring_preparation_contained(entry.tuple.reason);
        case PairwiseMonitoringProducerFamily::service_containment: {
            auto failure = monitoring_service_contained_failure(
                entry.tuple.reason == PairwiseFailureReason::contained_unknown_failure);
            return std::get<PairwiseMonitoringOperationFailure>(failure.primary());
        }
        case PairwiseMonitoringProducerFamily::stop_containment:
            return monitoring_stop_contained(entry.tuple.reason);
        case PairwiseMonitoringProducerFamily::service_placement:
            return monitoring_missing_service_binding_failure();
        case PairwiseMonitoringProducerFamily::scheduled_capacity: {
            auto failure = monitoring_schedule_capacity_failure();
            return std::get<PairwiseMonitoringOperationFailure>(failure.primary());
        }
        case PairwiseMonitoringProducerFamily::driver_transition: {
            if (entry.tuple.reason == PairwiseFailureReason::deadline_overflow) {
                auto failure = monitoring_deadline_failure();
                return std::get<PairwiseMonitoringOperationFailure>(failure.primary());
            }
            const auto phase = entry.tuple.reason == PairwiseFailureReason::abort
                ? PairwiseLifecyclePhase::abort_capture
                : entry.tuple.reason == PairwiseFailureReason::complete
                ? PairwiseLifecyclePhase::complete_capture
                : entry.tuple.reason == PairwiseFailureReason::stop_join_active_capture
                ? PairwiseLifecyclePhase::stop_and_join
                : PairwiseLifecyclePhase::begin_capture;
            auto failure = monitoring_driver_transition_failure(
                entry.tuple.code, entry.tuple.reason, phase);
            return std::get<PairwiseMonitoringOperationFailure>(failure.primary());
        }
        default: std::terminate();
        }
        std::terminate();
    }();
    return authored_tuple(value);
}

PairwiseMonitoringTerminalCapture::PairwiseMonitoringTerminalCapture(
    PairwiseMonitoringTerminalCapture&& other) noexcept
    : slot_{std::move(other.slot_)},
      result_taken_{std::exchange(other.result_taken_, true)} {}
PairwiseMonitoringTerminalCapture::~PairwiseMonitoringTerminalCapture() noexcept = default;

auto PairwiseMonitoringActiveCapture::registration() const noexcept
    -> PairwiseMonitoringCaptureRegistration* {
    return registrations_ ? registrations_.get() + registration_index_
                          : nullptr;
}

PairwiseMonitoringActiveCapture::~PairwiseMonitoringActiveCapture() noexcept {
    auto* const value = registration();
    if (!value)
        return;
    const auto current = value->state();
    if (current == PairwiseMonitoringCaptureRegistrationState::active)
        std::terminate();
    if (current == PairwiseMonitoringCaptureRegistrationState::retired)
        if (!value->unregister(generation_))
            std::terminate();
}

auto PairwiseMonitoringPreparationTestAccess::
    invariant_stop_outcome_for_projection() noexcept
    -> PairwiseMonitoringStopOutcome {
    return PairwiseMonitoringStopOutcome{{}, 0U, {}, 0U, 0U, {}, 0U, true};
}

} // namespace epycbench::runtime::detail
