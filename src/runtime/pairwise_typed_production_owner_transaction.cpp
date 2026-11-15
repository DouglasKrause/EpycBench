#include "runtime/pairwise_v2_progress.hpp"
#include "runtime/kernel_module_session.hpp"
#include "runtime/pairwise_typed_production_binding_preparation.hpp"
#include "runtime/pairwise_production_run_core.hpp"
#include "runtime/pairwise_measurement_leaf_facade.hpp"
#include "kernel/kernel_leaf_access.hpp"
#include "memory/region_finalization_access.hpp"

#include <optional>
#include <type_traits>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] auto transaction_failure(PairwiseFailureReason reason) noexcept
    -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations, reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value)
        std::terminate();
    return std::move(*value);
}

class TimedCompletionGuard final {
  public:
    explicit TimedCompletionGuard(
        const PairwiseWorkerExecutionControl& control) noexcept
        : control_{control} {}
    ~TimedCompletionGuard() noexcept {
        if (!completed_)
            control_.signal_timed_completion();
    }
    void complete() noexcept {
        control_.signal_timed_completion();
        completed_ = true;
    }
  private:
    const PairwiseWorkerExecutionControl& control_;
    bool completed_{};
};

} // namespace

class PairwiseTypedOwnerTransaction final {
  public:
    explicit PairwiseTypedOwnerTransaction(PairwiseTypedPreOwnerPreparation&& prepared,
                                           PairwiseProgressState* progress = nullptr)
        : progress_{progress}, cancellation_{std::move(prepared.cancellation_)},
          shared_cancellation_{std::move(prepared.shared_cancellation_)},
          worker_specs_{std::move(prepared.worker_specs_)}, calibration_{prepared.calibration_},
          warmup_{prepared.warmup_}, repetition_{prepared.repetition_},
          comparison_order_{prepared.comparison_order_}, validity_{prepared.validity_},
          calibration_enabled_{prepared.calibration_enabled_},
          measurement_{std::move(prepared.measurement_)},
          prepared_team_{std::move(prepared.worker_team_)},
          prepared_monitoring_{std::move(prepared.monitoring_)},
          first_inputs_{std::move(prepared.first_memory_)},
          second_inputs_{std::move(prepared.second_memory_)},
          first_owners_{std::move(prepared.first_memory_owners_)},
          second_owners_{std::move(prepared.second_memory_owners_)},
          initializer_{std::move(prepared.initializer_)}, platform_{std::move(prepared.platform_)},
          kernel_{std::move(prepared.kernel_)},
          worker_dependencies_{std::move(prepared.worker_dependencies_)},
          first_request_{prepared.first_request_}, second_request_{prepared.second_request_},
          first_identity_{std::move(prepared.first_identity_)},
          second_identity_{std::move(prepared.second_identity_)}, family_{prepared.family_},
          comparison_order_seed_{prepared.comparison_order_seed_},
          monitoring_policy_{std::move(prepared.monitoring_policy_)},
          service_placement_{std::move(prepared.service_placement_)},
          statistics_policy_{std::move(prepared.statistics_policy_)},
          maximum_blocks_{prepared.maximum_blocks_},
          bandwidth_configuration_{std::move(prepared.bandwidth_configuration_)},
          latency_configuration_{std::move(prepared.latency_configuration_)},
          latency_verification_{std::move(prepared.latency_verification_)},
          implementation_id_{prepared.implementation_id_},
          resolved_latency_seed_{prepared.resolved_latency_seed_},
          measurement_root_seed_{prepared.measurement_root_seed_},
          calibration_experiment_id_{prepared.calibration_experiment_id_},
          calibration_block_id_{prepared.calibration_block_id_},
          operation_secondary_{std::move(prepared.operation_secondary_)},
          first_finalization_slots_{std::move(prepared.first_finalization_slots_)},
          second_finalization_slots_{std::move(prepared.second_finalization_slots_)},
          first_finalized_workers_{std::move(prepared.first_finalized_workers_)},
          second_finalized_workers_{std::move(prepared.second_finalized_workers_)},
          finalization_associations_{std::move(prepared.finalization_associations_)},
          preparation_graph_{std::move(prepared.preparation_graph_)} {
        if (progress_) static_cast<void>(progress_->bind(this));
        worker_ids_.reserve(worker_specs_.size());
        for (const auto& worker : worker_specs_)
            worker_ids_.push_back(worker.worker_id);
        prepared.active_ = false;
    }
    PairwiseTypedOwnerTransaction(const PairwiseTypedOwnerTransaction&) = delete;
    PairwiseTypedOwnerTransaction(PairwiseTypedOwnerTransaction&&) = delete;
    ~PairwiseTypedOwnerTransaction() noexcept {
        retire_post_transfer();
        if (preparation_graph_) preparation_graph_->retire();
    }

    [[nodiscard]] auto acquire(PairwiseTypedOwnerStage stage,
                               PairwiseTypedFailureSink& sink) noexcept
        -> std::expected<void, PairwiseEvidenceFailure> {
        switch (stage) {
        case PairwiseTypedOwnerStage::worker_team:
            return acquire_team(sink);
        case PairwiseTypedOwnerStage::first_memory:
            return acquire_memory(first_inputs_, first_owners_, sink);
        case PairwiseTypedOwnerStage::second_memory:
            if (auto acquired = acquire_memory(second_inputs_, second_owners_, sink);
                !acquired)
                return acquired;
            if (auto materialized=materialize_kernels(); !materialized) {
                // The adapter rolls back only stages preceding this failed stage.
                retire_memory(second_inputs_, second_owners_, sink);
                return materialized;
            }
            if (preparation_graph_) {
                for (std::size_t index{};index<first_owners_.size();++index) {
                    const bool latency=family_==PairwiseTypedPreparedFamily::latency;
                    if (!(latency ? first_owners_[index].latency.has_value() && second_owners_[index].latency.has_value() &&
                                      !first_owners_[index].bandwidth && !second_owners_[index].bandwidth && latency_configuration_ &&
                                      first_owners_[index].latency->input().configuration()==*latency_configuration_ &&
                                      second_owners_[index].latency->input().configuration()==*latency_configuration_
                                  : first_owners_[index].bandwidth.has_value() && second_owners_[index].bandwidth.has_value() &&
                                      !first_owners_[index].latency && !second_owners_[index].latency && bandwidth_configuration_ &&
                                      first_owners_[index].bandwidth->input().configuration()==*bandwidth_configuration_ &&
                                      second_owners_[index].bandwidth->input().configuration()==*bandwidth_configuration_) ||
                        !preparation_graph_->capture_kernel(false,index,latency) ||
                        !preparation_graph_->capture_kernel(true,index,latency)) {
                        retire_memory(second_inputs_, second_owners_, sink);
                        return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
                    }
                }
                if (!preparation_graph_->transition(PreparationGraphStage::kernels)) {
                    retire_memory(second_inputs_, second_owners_, sink);
                    return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
                }
            }
            return {};
        case PairwiseTypedOwnerStage::monitoring:
            return acquire_monitoring(sink);
        case PairwiseTypedOwnerStage::ownership_transfer:
            if (!team_ || !monitoring_ || !all_owned(first_owners_) || !all_owned(second_owners_) ||
                transferred_)
                return std::unexpected{
                    transaction_failure(PairwiseFailureReason::contained_exception)};
            if (preparation_graph_ && !preparation_graph_->transition(PreparationGraphStage::transferred))
                return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
            transferred_ = true;
            progress_quiesce();
            return {};
        case PairwiseTypedOwnerStage::count:
            break;
        }
        return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
    }

    [[nodiscard]] auto finalize() noexcept -> PairwiseFinalizationSource {
        ProgressOperation progress{*this};
        if (!transferred_ || !team_ || !monitoring_ ||
            first_finalization_slots_.size() != worker_ids_.size() ||
            second_finalization_slots_.size() != worker_ids_.size() ||
            !first_finalized_workers_.empty() ||
            !second_finalized_workers_.empty() || !finalization_associations_.empty())
            std::terminate();
        // An already-materialized failed operation may leave its borrowed
        // secondary view populated.  The core owns that copied failure now;
        // this fixed-capacity destination is reused only for finalization-local
        // failures.
        operation_secondary_.clear();

        PairwiseMemoryOwningFacade memory{platform_, kernel_};
        const auto finalize_variant = [&](
            measurement::ComparisonVariantSlot variant,
            auto& inputs, auto& owners, auto& slots, auto& finalized) noexcept {
            if (inputs.size() != owners.size() || owners.size() != slots.size())
                std::terminate();
            for (std::size_t index{}; index < owners.size(); ++index) {
                auto& owner = owners[index];
                if (!owner.owner)
                    std::terminate();
                owner.bandwidth.reset();
                owner.latency.reset();
                auto result = memory.finalize(
                    std::move(*owner.owner),
                    memory::PostRunVerificationApplicability::applicable,
                    inputs[index].workspace);
                owner.owner.reset();
                auto& slot = slots[index];
                if (result) {
                    slot.diagnostic = memory::detail::RegionFinalizationAccess::
                        take_diagnostic(std::move(*result));
                    if (!slot.diagnostic)
                        std::terminate();
                } else {
                    auto projected = project_pairwise_memory_finalization_failure(
                        std::move(result.error()));
                    slot.diagnostic.emplace(std::move(projected.diagnostic));
                    slot.primary_error_ordinal = operation_secondary_.size();
                    append_secondary(PairwiseEvidenceFailure{
                        std::move(projected.primary)});
                    if (projected.cleanup) {
                        slot.cleanup_error_ordinal = operation_secondary_.size();
                        append_secondary(PairwiseEvidenceFailure{
                            std::move(*projected.cleanup)});
                    }
                    finalization_associations_.push_back({
                        variant, index, slot.primary_error_ordinal,
                        slot.cleanup_error_ordinal});
                }
                finalized.push_back({worker_ids_[index],
                                     std::move(*slot.diagnostic),
                                     std::nullopt, std::nullopt});
                slot.diagnostic.reset();
            }
        };

        finalize_variant(measurement::ComparisonVariantSlot::first,
                         first_inputs_, first_owners_,
                         first_finalization_slots_, first_finalized_workers_);
        finalize_variant(measurement::ComparisonVariantSlot::second,
                         second_inputs_, second_owners_,
                         second_finalization_slots_, second_finalized_workers_);

        auto monitoring_cursor = PairwiseMonitoringFailureCursor::consume(
            monitoring_->stop_and_retire(platform_));
        const bool monitoring_retired = monitoring_->progress_quiescent(true);
        while (!monitoring_cursor.empty())
            append_secondary(monitoring_cursor.next());

        auto stopped = team_->stop_and_join();
        if (!stopped) {
            append_secondary(PairwiseEvidenceFailure{
                std::move(stopped.error().primary)});
            for (auto& cleanup : stopped.error().cleanup)
                append_secondary(PairwiseEvidenceFailure{std::move(cleanup)});
        }
        if (team_->active())
            std::terminate();
        if (progress_ && monitoring_retired && team_->progress_quiescent()) progress_->seal(this);
        monitoring_.reset();
        team_.reset();
        transferred_ = false;
        if (preparation_graph_) preparation_graph_->retire();
        return {
            PairwiseFinalizationEvidence{
                {measurement::ComparisonVariantSlot::first,
                 std::move(first_finalized_workers_)},
                {measurement::ComparisonVariantSlot::second,
                 std::move(second_finalized_workers_)}},
            finalization_associations_, operation_secondary_};
    }

    void cleanup(PairwiseTypedOwnerStage stage, PairwiseTypedFailureSink& sink) noexcept {
        if (transferred_)
            return;
        switch (stage) {
        case PairwiseTypedOwnerStage::monitoring:
            retire_monitoring(sink);
            break;
        case PairwiseTypedOwnerStage::second_memory:
            retire_memory(second_inputs_, second_owners_, sink);
            break;
        case PairwiseTypedOwnerStage::first_memory:
            retire_memory(first_inputs_, first_owners_, sink);
            break;
        case PairwiseTypedOwnerStage::worker_team:
            retire_team(sink);
            break;
        case PairwiseTypedOwnerStage::ownership_transfer:
        case PairwiseTypedOwnerStage::count:
            break;
        }
    }

    [[nodiscard]] auto admit_observation(
        const measurement::ScheduledObservationSlot& slot) const noexcept -> bool {
        if (slot.block_id().value() >= maximum_blocks_ || slot.slot_index() >= 4U)
            return false;
        const auto expected = measurement::MeasurementBlockPlan::create(
            slot.block_id(), comparison_order_,
            measurement::BlockPreparationPlan::create(false, false));
        return expected.observations()[slot.slot_index()] == slot;
    }

    void bind_prepared_owners(PairwiseTypedPreparedOwners* owners) noexcept {
        if (owners == nullptr)
            std::terminate();
        bound_owners_ = owners;
    }
    [[nodiscard]] auto bound_owners() noexcept -> PairwiseTypedPreparedOwners& {
        if (bound_owners_ == nullptr)
            std::terminate();
        return *bound_owners_;
    }
    [[nodiscard]] auto matches(
        const measurement::CalibrationDurationPolicy& policy) const noexcept {
        return policy.minimum_duration() == calibration_.minimum_duration() &&
               policy.target_duration() == calibration_.target_duration() &&
               policy.maximum_duration() == calibration_.maximum_duration();
    }
    [[nodiscard]] auto matches(
        const measurement::WarmupPolicy& policy) const noexcept {
        const auto left = policy.stability_threshold();
        const auto right = warmup_.stability_threshold();
        return policy.minimum_trials() == warmup_.minimum_trials() &&
               policy.maximum_trials() == warmup_.maximum_trials() &&
               policy.stability_window() == warmup_.stability_window() &&
               left.numerator() == right.numerator() &&
               left.denominator() == right.denominator();
    }

    [[nodiscard]] auto calibrate_bandwidth() noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::SequentialReadBandwidthCalibrationOutcome> {
        ProgressOperation progress{*this};
        if (!progress.admitted)
            return source_failure<measurement::SequentialReadBandwidthCalibrationOutcome>(transaction_failure(PairwiseFailureReason::contained_exception));
        if (family_ != PairwiseTypedPreparedFamily::bandwidth)
            std::terminate();
        auto& prepared = std::get<PairwisePreparedBandwidthMeasurementExecution>(measurement_);
        return PairwiseMeasurementLeafFacade{}.calibrate(
            prepared, calibration_,
            {this, empty_trial,
             [](void* context, auto work) noexcept {
                 return static_cast<PairwiseTypedOwnerTransaction*>(context)
                     ->run_bandwidth(true, work,
                                     PairwiseWorkerGenerationClass::calibration);
             }});
    }

    [[nodiscard]] auto calibrate_latency() noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::DependentLoadLatencyCalibrationOutcome> {
        ProgressOperation progress{*this};
        if (!progress.admitted)
            return source_failure<measurement::DependentLoadLatencyCalibrationOutcome>(transaction_failure(PairwiseFailureReason::contained_exception));
        if (family_ != PairwiseTypedPreparedFamily::latency)
            std::terminate();
        auto& prepared = std::get<PairwisePreparedLatencyMeasurementExecution>(measurement_);
        return PairwiseMeasurementLeafFacade{}.calibrate(
            prepared, calibration_,
            {this, empty_trial,
             [](void* context, auto work) noexcept {
                 return static_cast<PairwiseTypedOwnerTransaction*>(context)
                     ->run_latency(true, work,
                                   PairwiseWorkerGenerationClass::calibration);
             }});
    }

    [[nodiscard]] auto warmup_bandwidth(
        kernel::SequentialReadBandwidthPassCount work) noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::SequentialReadBandwidthWarmupOutcome> {
        ProgressOperation progress{*this};
        if (!progress.admitted)
            return source_failure<measurement::SequentialReadBandwidthWarmupOutcome>(transaction_failure(PairwiseFailureReason::contained_exception));
        if (family_ != PairwiseTypedPreparedFamily::bandwidth)
            std::terminate();
        auto& prepared = std::get<PairwisePreparedBandwidthMeasurementExecution>(measurement_);
        return PairwiseMeasurementLeafFacade{}.warmup(
            prepared, warmup_, work,
            {this, [](void* context, auto selected) noexcept {
                 auto& self = *static_cast<PairwiseTypedOwnerTransaction*>(context);
                 auto first = self.run_bandwidth(
                     true, selected, PairwiseWorkerGenerationClass::warmup);
                 if (!first)
                     return first;
                 auto second = self.run_bandwidth(
                     false, selected, PairwiseWorkerGenerationClass::warmup);
                 if (!second)
                     return second;
                 return self.interval(first->start(), second->end());
             }});
    }

    [[nodiscard]] auto warmup_latency(
        kernel::DependentLoadLatencyStepCount work) noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::DependentLoadLatencyWarmupOutcome> {
        ProgressOperation progress{*this};
        if (!progress.admitted)
            return source_failure<measurement::DependentLoadLatencyWarmupOutcome>(transaction_failure(PairwiseFailureReason::contained_exception));
        if (family_ != PairwiseTypedPreparedFamily::latency)
            std::terminate();
        auto& prepared = std::get<PairwisePreparedLatencyMeasurementExecution>(measurement_);
        return PairwiseMeasurementLeafFacade{}.warmup(
            prepared, warmup_, work,
            {this, [](void* context, auto selected) noexcept {
                 auto& self = *static_cast<PairwiseTypedOwnerTransaction*>(context);
                 auto first = self.run_latency(
                     true, selected, PairwiseWorkerGenerationClass::warmup);
                 if (!first)
                     return first;
                 auto second = self.run_latency(
                     false, selected, PairwiseWorkerGenerationClass::warmup);
                 if (!second)
                     return second;
                 return self.interval(first->start(), second->end());
             }});
    }

    [[nodiscard]] auto observe_bandwidth(
        const measurement::SequentialReadBandwidthObservationRequiredAction& action,
        PairwiseTypedObservationDestination& destination) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
        ProgressOperation progress{*this};
        if (!progress.admitted)
            return source_failure<FactualObservationEvidence>(transaction_failure(PairwiseFailureReason::contained_exception));
        if (family_ != PairwiseTypedPreparedFamily::bandwidth ||
            !bandwidth_configuration_)
            std::terminate();
        return observe(action, destination, action.selected_state().selected_work());
    }

    [[nodiscard]] auto observe_latency(
        const measurement::DependentLoadLatencyObservationRequiredAction& action,
        PairwiseTypedObservationDestination& destination) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
        ProgressOperation progress{*this};
        if (!progress.admitted)
            return source_failure<FactualObservationEvidence>(transaction_failure(PairwiseFailureReason::contained_exception));
        if (family_ != PairwiseTypedPreparedFamily::latency ||
            !latency_configuration_)
            std::terminate();
        return observe(action, destination, action.selected_state().selected_work());
    }

  private:
    void progress_quiesce() noexcept {
        if (progress_ && team_ && monitoring_ && team_->progress_quiescent() &&
            monitoring_->progress_quiescent()) progress_->quiesce(this);
    }
    struct ProgressOperation final {
        PairwiseTypedOwnerTransaction& owner;
        bool admitted;
        explicit ProgressOperation(PairwiseTypedOwnerTransaction& value) noexcept
            : owner{value}, admitted{!owner.progress_ || owner.progress_->begin_operation(&owner)} {}
        ~ProgressOperation() noexcept { owner.progress_quiesce(); }
    };
    [[nodiscard]] auto clock(PairwiseTypedClockSite site, bool first,
                             std::optional<core::WorkerId> worker = std::nullopt) noexcept {
        if (worker_dependencies_.hooks.before_clock != nullptr)
            worker_dependencies_.hooks.before_clock(
                worker_dependencies_.hooks.context, site, first, worker);
        return platform_.raw_monotonic_now();
    }
    template <class T>
    [[nodiscard]] auto source_failure(PairwiseEvidenceFailure primary) noexcept
        -> PairwiseTypedOperationSourceResult<T> {
        return std::unexpected{PairwiseTypedOperationFailureSource{
            std::move(primary), operation_secondary_, std::nullopt}};
    }

    template <class T>
    [[nodiscard]] auto observation_failure(
        PairwiseEvidenceFailure primary,
        kernel::DependentLoadLatencyBlockId block) noexcept
        -> PairwiseTypedOperationSourceResult<T> {
        return std::unexpected{PairwiseTypedOperationFailureSource{
            std::move(primary), operation_secondary_, block}};
    }

    void append_capture_failure(PairwiseMonitoringCaptureFailure failure) noexcept {
        auto cursor = PairwiseMonitoringCaptureFailureCursor::consume(std::move(failure));
        while (!cursor.empty()) append_secondary(cursor.next());
    }

    static auto monitoring_evidence(
        const monitoring::MonitoringCaptureResult& capture) noexcept
        -> measurement::ObservationMonitoringEvidence {
        const auto signal = [](monitoring::MonitoringSignalState state) noexcept {
            using From = monitoring::MonitoringSignalState;
            using To = measurement::ObservationMonitoringSignalState;
            switch (state) {
            case From::clear: return To::clear;
            case From::detected: return To::detected;
            case From::unavailable: return To::unavailable;
            case From::not_applicable: return To::not_applicable;
            }
            std::terminate();
        };
        const auto& evidence = capture.evidence();
        return measurement::ObservationMonitoringEvidence::create(
            signal(evidence.page_faults_state()),
            signal(evidence.throttling_state()),
            signal(evidence.context_switches_state()),
            signal(evidence.frequency_state()),
            signal(evidence.temperature_state()));
    }

    [[nodiscard]] auto interval(platform_linux::RawMonotonicTimestamp start,
                                platform_linux::RawMonotonicTimestamp end) noexcept
        -> PairwiseTypedOperationSourceResult<measurement::MeasurementInterval> {
        auto result = PairwiseMeasurementLeafFacade{}.measurement_interval(
            start, end, PairwiseVerificationPhase::before);
        if (!result)
            return source_failure<measurement::MeasurementInterval>(
                PairwiseEvidenceFailure{std::move(result.error())});
        return std::move(*result);
    }

    static auto empty_trial(void* opaque) noexcept
        -> PairwiseTypedOperationSourceResult<measurement::EmptyHarnessTrial> {
        auto& self = *static_cast<PairwiseTypedOwnerTransaction*>(opaque);
        self.operation_secondary_.clear();
        auto start = self.clock(PairwiseTypedClockSite::empty_start, true);
        if (!start)
            return self.source_failure<measurement::EmptyHarnessTrial>(
                PairwiseEvidenceFailure{std::move(start.error())});
        auto end = self.clock(PairwiseTypedClockSite::empty_end, true);
        if (!end)
            return self.source_failure<measurement::EmptyHarnessTrial>(
                PairwiseEvidenceFailure{std::move(end.error())});
        auto measured = self.interval(*start, *end);
        if (!measured)
            return std::unexpected{std::move(measured.error())};
        return measurement::EmptyHarnessTrial::create(std::move(*measured));
    }

    template <class Work>
    [[nodiscard]] auto collect_generation(
        bool first, Work work, PairwiseWorkerGenerationClass generation_class,
        PairwiseWorkerOperation operation) noexcept
        -> PairwiseTypedOperationSourceResult<measurement::MeasurementInterval> {
        operation_secondary_.clear();
        if (progress_) progress_->invalidate(this);
        auto generation = team_->begin_generation(
            generation_class, operation, WorkerObservationPolicy::none,
            WorkerCancellationPolicy::cleanup_insensitive);
        if (!generation)
            return source_failure<measurement::MeasurementInterval>(
                PairwiseEvidenceFailure{std::move(generation.error())});
        auto ready = generation->await_ready();
        if (!ready)
            return source_failure<measurement::MeasurementInterval>(
                PairwiseEvidenceFailure{std::move(ready.error())});
        auto enclosing_start = clock(PairwiseTypedClockSite::controller_start,
                                     first);
        if (!enclosing_start) {
            auto primary = PairwiseEvidenceFailure{
                std::move(enclosing_start.error())};
            generation->cancel_before_release();
            static_cast<void>(generation->await_quiescence());
            return source_failure<measurement::MeasurementInterval>(
                std::move(primary));
        }
        generation->release();
        // The ordered outcome below is the sole worker-failure authority;
        // staged wait errors are coordination echoes of that same outcome.
        auto timed = generation->await_timed_completion();
        auto enclosing_end = clock(PairwiseTypedClockSite::controller_end, first);
        auto quiescent = generation->await_quiescence();
        auto outcome = generation->collect_ordered_results();
        std::optional<PairwiseEvidenceFailure> primary;
        if (outcome) {
            for (const auto& worker : outcome->workers()) {
                std::optional<PairwiseEvidenceFailure> value;
                if (worker.operation_primary)
                    value.emplace(*worker.operation_primary);
                else if (worker.failure)
                    value.emplace(*worker.failure);
                if (value) {
                    if (!primary)
                        primary.emplace(std::move(*value));
                    else
                        append_secondary(std::move(*value));
                }
                if (worker.operation_cleanup)
                    append_secondary(*worker.operation_cleanup);
            }
        }
        const auto record_failure = [&](PairwiseEvidenceFailure failure) {
            if (!primary)
                primary.emplace(std::move(failure));
            else
                append_secondary(std::move(failure));
        };
        if (!outcome) {
            if (!timed)
                record_failure(PairwiseEvidenceFailure{
                    std::move(timed.error())});
            else if (!quiescent)
                record_failure(PairwiseEvidenceFailure{
                    std::move(quiescent.error())});
            if (!enclosing_end)
                record_failure(PairwiseEvidenceFailure{
                    std::move(enclosing_end.error())});
            record_failure(PairwiseEvidenceFailure{
                std::move(outcome.error())});
        } else if (!enclosing_end) {
            record_failure(PairwiseEvidenceFailure{
                std::move(enclosing_end.error())});
        }
        if (primary)
            return source_failure<measurement::MeasurementInterval>(
                std::move(*primary));
        (void)first;
        (void)work;
        return interval(*enclosing_start, *enclosing_end);
    }

    void append_secondary(PairwiseEvidenceFailure value) noexcept {
        if (operation_secondary_.size() >= operation_secondary_.capacity())
            std::terminate();
        operation_secondary_.push_back(std::move(value));
    }

    [[nodiscard]] auto run_bandwidth(
        bool first, kernel::SequentialReadBandwidthPassCount work,
        PairwiseWorkerGenerationClass generation_class) noexcept
        -> PairwiseTypedOperationSourceResult<measurement::MeasurementInterval> {
        struct Command final {
            PairwiseTypedOwnerTransaction* self;
            std::vector<PairwisePreparedMemoryOwnerDestination>* destinations;
            kernel::SequentialReadBandwidthPassCount work;
            bool first;
        } command{this, first ? &first_owners_ : &second_owners_, work, first};
        const auto callback = [](void* opaque, const WorkerExecutionContext& worker,
                                 const PairwiseWorkerExecutionControl& control) noexcept
            -> PairwiseWorkerCallbackResult {
            auto& state = *static_cast<Command*>(opaque);
            TimedCompletionGuard timed_completion{control};
            if (worker.worker_index >= state.destinations->size())
                std::terminate();
            auto& destination = (*state.destinations)[worker.worker_index];
            if (!destination.bandwidth)
                std::terminate();
            const auto fail = [](PairwiseEvidenceFailure value)
                -> PairwiseWorkerCallbackResult {
                auto typed = PairwiseWorkerOperationFailure::create(std::move(value));
                if (!typed)
                    std::terminate();
                return std::unexpected{PairwiseWorkerCallbackFailure{
                    std::move(*typed), std::nullopt}};
            };
            auto start = state.self->clock(PairwiseTypedClockSite::worker_start,
                                           state.first, worker.worker_id);
            if (!start)
                return fail(PairwiseEvidenceFailure{std::move(start.error())});
            destination.last_start = *start;
            const auto terminal = kernel::execute_sequential_read_bandwidth(
                *destination.bandwidth, state.work);
            auto end = state.self->clock(PairwiseTypedClockSite::worker_end,
                                         state.first, worker.worker_id);
            if (!end)
                return fail(PairwiseEvidenceFailure{std::move(end.error())});
            destination.last_end = *end;
            timed_completion.complete();
            auto accounted = state.self->kernel_.bandwidth_work(
                *destination.bandwidth, state.work);
            if (!accounted)
                return fail(PairwiseEvidenceFailure{std::move(accounted.error())});
            const auto checksum =
                kernel::PreparedSequentialReadBandwidthChecksum::prepare(
                    *destination.bandwidth);
            const auto verification = kernel::verify_sequential_read_bandwidth_checksum(
                checksum.expected_terminal(state.work), terminal);
            if (!module_verification_matches(*destination.bandwidth, verification))
                return fail(transaction_failure(PairwiseFailureReason::contained_exception));
            destination.last_work_units = state.work.value();
            destination.last_logical_bytes =
                accounted->logical_bytes_read().value();
            destination.last_bandwidth_work.emplace(*accounted);
            destination.last_expected_terminal = verification.expected().value();
            destination.last_actual_terminal = verification.actual().value();
            destination.last_verification_matched = verification.matched();
            destination.last_bandwidth_verification.emplace(verification);
            return {};
        };
        return collect_generation(first, work, generation_class,
                                  {&command, callback});
    }

    [[nodiscard]] auto run_latency(
        bool first, kernel::DependentLoadLatencyStepCount work,
        PairwiseWorkerGenerationClass generation_class) noexcept
        -> PairwiseTypedOperationSourceResult<measurement::MeasurementInterval> {
        struct Command final {
            PairwiseTypedOwnerTransaction* self;
            std::vector<PairwisePreparedMemoryOwnerDestination>* destinations;
            kernel::DependentLoadLatencyStepCount work;
            bool first;
        } command{this, first ? &first_owners_ : &second_owners_, work, first};
        const auto callback = [](void* opaque, const WorkerExecutionContext& worker,
                                 const PairwiseWorkerExecutionControl& control) noexcept
            -> PairwiseWorkerCallbackResult {
            auto& state = *static_cast<Command*>(opaque);
            TimedCompletionGuard timed_completion{control};
            if (worker.worker_index >= state.destinations->size() ||
                !state.self->latency_verification_)
                std::terminate();
            auto& destination = (*state.destinations)[worker.worker_index];
            if (!destination.latency)
                std::terminate();
            const auto fail = [](PairwiseEvidenceFailure value)
                -> PairwiseWorkerCallbackResult {
                auto typed = PairwiseWorkerOperationFailure::create(std::move(value));
                if (!typed)
                    std::terminate();
                return std::unexpected{PairwiseWorkerCallbackFailure{
                    std::move(*typed), std::nullopt}};
            };
            auto seed = state.self->kernel_.prepare_latency_schedule_seed(
                state.self->measurement_root_seed_,
                state.self->calibration_experiment_id_,
                state.self->calibration_block_id_);
            if (!seed)
                return fail(PairwiseEvidenceFailure{std::move(seed.error())});
            auto schedule = state.self->kernel_.latency_schedule(
                *destination.latency, *seed, state.work);
            if (!schedule)
                return fail(PairwiseEvidenceFailure{std::move(schedule.error())});
            auto start = state.self->clock(PairwiseTypedClockSite::worker_start,
                                           state.first, worker.worker_id);
            if (!start)
                return fail(PairwiseEvidenceFailure{std::move(start.error())});
            destination.last_start = *start;
            const auto terminal = kernel::execute_dependent_load_latency(
                *destination.latency, *schedule);
            auto end = state.self->clock(PairwiseTypedClockSite::worker_end,
                                         state.first, worker.worker_id);
            if (!end)
                return fail(PairwiseEvidenceFailure{std::move(end.error())});
            destination.last_end = *end;
            timed_completion.complete();
            const auto verification = kernel::verify_dependent_load_latency_terminal(
                state.self->latency_verification_->expected_terminal(*schedule),
                terminal);
            if (!module_verification_matches(*destination.latency, verification))
                return fail(transaction_failure(PairwiseFailureReason::contained_exception));
            destination.last_work_units = state.work.value();
            destination.last_logical_bytes = 0U;
            destination.last_expected_terminal =
                static_cast<std::uint64_t>(verification.expected().value());
            destination.last_actual_terminal =
                static_cast<std::uint64_t>(verification.actual().value());
            destination.last_verification_matched = verification.matched();
            destination.last_latency_verification.emplace(verification);
            destination.last_latency_schedule.emplace(*schedule);
            return {};
        };
        return collect_generation(first, work, generation_class,
                                  {&command, callback});
    }

    template <class Action, class Work>
    [[nodiscard]] auto observe(
        const Action& action, PairwiseTypedObservationDestination& destination,
        Work work) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
        operation_secondary_.clear();
        const auto block = action.block_plan().block_id();
        const bool first = action.slot().comparison_variant() ==
                           measurement::ComparisonVariantSlot::first;
        auto& owners = first ? first_owners_ : second_owners_;
        auto append_capture = [&](PairwiseMonitoringCaptureFailure failure) {
            auto cursor = PairwiseMonitoringCaptureFailureCursor::consume(
                std::move(failure));
            while (!cursor.empty()) append_secondary(cursor.next());
        };
        struct Command final {
            PairwiseTypedOwnerTransaction* self;
            std::vector<PairwisePreparedMemoryOwnerDestination>* owners;
            Work work;
            bool first;
            core::ExperimentId experiment;
            kernel::DependentLoadLatencyBlockId block;
        } command{this, &owners, work, first, action.experiment_id(), block};
        const auto callback = [](void* opaque, const WorkerExecutionContext& worker,
                                 const PairwiseWorkerExecutionControl& control) noexcept
            -> PairwiseWorkerCallbackResult {
            auto& state = *static_cast<Command*>(opaque);
            TimedCompletionGuard timed_completion{control};
            auto& owner = (*state.owners)[worker.worker_index];
            const auto fail = [](PairwiseEvidenceFailure value)
                -> PairwiseWorkerCallbackResult {
                auto typed = PairwiseWorkerOperationFailure::create(std::move(value));
                if (!typed) std::terminate();
                return std::unexpected{PairwiseWorkerCallbackFailure{
                    std::move(*typed), std::nullopt}};
            };
            if constexpr (std::is_same_v<Work,
                          kernel::SequentialReadBandwidthPassCount>) {
                if (!owner.bandwidth) std::terminate();
                auto start = state.self->clock(PairwiseTypedClockSite::worker_start,
                                               state.first, worker.worker_id);
                if (!start) return fail(PairwiseEvidenceFailure{std::move(start.error())});
                owner.last_start = *start;
                const auto terminal = kernel::execute_sequential_read_bandwidth(
                    *owner.bandwidth, state.work);
                auto end = state.self->clock(PairwiseTypedClockSite::worker_end,
                                             state.first, worker.worker_id);
                if (!end) return fail(PairwiseEvidenceFailure{std::move(end.error())});
                owner.last_end = *end;
                timed_completion.complete();
                auto accounted = state.self->kernel_.bandwidth_work(
                    *owner.bandwidth, state.work);
                if (!accounted)
                    return fail(PairwiseEvidenceFailure{std::move(accounted.error())});
                const auto checksum =
                    kernel::PreparedSequentialReadBandwidthChecksum::prepare(
                        *owner.bandwidth);
                const auto verification =
                    kernel::verify_sequential_read_bandwidth_checksum(
                        checksum.expected_terminal(state.work), terminal);
                if (!module_verification_matches(*owner.bandwidth, verification))
                    return fail(transaction_failure(PairwiseFailureReason::contained_exception));
                owner.last_logical_bytes = accounted->logical_bytes_read().value();
                owner.last_bandwidth_work.emplace(*accounted);
                owner.last_expected_terminal = verification.expected().value();
                owner.last_actual_terminal = verification.actual().value();
                owner.last_verification_matched = verification.matched();
                owner.last_bandwidth_verification.emplace(verification);
            } else {
                if (!owner.latency || !state.self->latency_verification_)
                    std::terminate();
                auto seed = state.self->kernel_.prepare_latency_schedule_seed(
                    state.self->resolved_latency_seed_, state.experiment, state.block);
                if (!seed) return fail(PairwiseEvidenceFailure{std::move(seed.error())});
                auto schedule = state.self->kernel_.latency_schedule(
                    *owner.latency, *seed, state.work);
                if (!schedule)
                    return fail(PairwiseEvidenceFailure{std::move(schedule.error())});
                auto start = state.self->clock(PairwiseTypedClockSite::worker_start,
                                               state.first, worker.worker_id);
                if (!start) return fail(PairwiseEvidenceFailure{std::move(start.error())});
                owner.last_start = *start;
                const auto terminal = kernel::execute_dependent_load_latency(
                    *owner.latency, *schedule);
                auto end = state.self->clock(PairwiseTypedClockSite::worker_end,
                                             state.first, worker.worker_id);
                if (!end) return fail(PairwiseEvidenceFailure{std::move(end.error())});
                owner.last_end = *end;
                timed_completion.complete();
                const auto verification =
                    kernel::verify_dependent_load_latency_terminal(
                        state.self->latency_verification_->expected_terminal(*schedule),
                        terminal);
                if (!module_verification_matches(*owner.latency, verification))
                    return fail(transaction_failure(PairwiseFailureReason::contained_exception));
                owner.last_expected_terminal = verification.expected().value();
                owner.last_actual_terminal = verification.actual().value();
                owner.last_verification_matched = verification.matched();
                owner.last_latency_verification.emplace(verification);
                owner.last_latency_schedule.emplace(*schedule);
            }
            owner.last_work_units = state.work.value();
            return {};
        };
        if (progress_) progress_->invalidate(this);
        auto generation = team_->begin_generation(
            PairwiseWorkerGenerationClass::observation, {&command, callback},
            WorkerObservationPolicy::before_and_after,
            WorkerCancellationPolicy::cleanup_insensitive);
        if (!generation)
            return observation_failure<FactualObservationEvidence>(
                PairwiseEvidenceFailure{std::move(generation.error())}, block);
        auto ready = generation->await_ready();
        if (!ready) {
            auto primary = PairwiseEvidenceFailure{std::move(ready.error())};
            generation->cancel_before_release();
            static_cast<void>(generation->await_quiescence());
            return observation_failure<FactualObservationEvidence>(std::move(primary), block);
        }
        if (progress_) progress_->invalidate(this);
        auto capture = monitoring_->begin_capture(platform_);
        if (!capture) {
            auto cursor = PairwiseMonitoringCaptureFailureCursor::consume(
                std::move(capture.error()));
            auto primary = cursor.next();
            while (!cursor.empty()) append_secondary(cursor.next());
            generation->cancel_before_release();
            static_cast<void>(generation->await_quiescence());
            return observation_failure<FactualObservationEvidence>(std::move(primary), block);
        }
        auto controller_start = clock(PairwiseTypedClockSite::controller_start, first);
        if (!controller_start) {
            auto primary = PairwiseEvidenceFailure{std::move(controller_start.error())};
            generation->cancel_before_release();
            static_cast<void>(generation->await_quiescence());
            if (auto aborted = monitoring_->abort_capture(std::move(*capture)); !aborted)
                append_capture(std::move(aborted.error()));
            return observation_failure<FactualObservationEvidence>(std::move(primary), block);
        }
        generation->release();
        auto timed = generation->await_timed_completion();
        const auto same = [](const PairwiseEvidenceFailure& left,
                             const PairwiseEvidenceFailure& right) noexcept {
            return std::visit([](const auto& a, const auto& b) noexcept {
                return a.code() == b.code() && a.operation() == b.operation() &&
                       a.reason() == b.reason() && a.context() == b.context() &&
                       a.posix_errno() == b.posix_errno();
            }, left, right);
        };
        // Consume raw Runtime command results only to retain distinct failure /
        // cleanup facts. MSR006 worker Observations are never constructed here.
        const auto failed_completion = [&](std::optional<PairwiseEvidenceFailure> primary,
                                           const PairwiseEvidenceFailure* awaited)
            -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
            auto quiescent = generation->await_quiescence();
            auto cleanup = generation->collect_ordered_results();
            const auto record = [&](PairwiseEvidenceFailure value) {
                if (!primary) primary.emplace(std::move(value));
                else append_secondary(std::move(value));
            };
            if (cleanup) {
                for (const auto& worker : cleanup->workers()) {
                    if (worker.operation_primary) record(*worker.operation_primary);
                    else if (worker.failure) record(*worker.failure);
                    if (worker.operation_cleanup) append_secondary(*worker.operation_cleanup);
                }
                if (awaited && !std::ranges::any_of(cleanup->workers(), [&](const auto& worker) {
                        return (worker.failure && same(*awaited, *worker.failure)) ||
                            (worker.operation_primary && same(*awaited, *worker.operation_primary));
                    })) record(*awaited);
            } else {
                // A failed consuming projection cannot replace an earlier timed
                // or quiescent failure whose actual worker result is unavailable.
                if (awaited) record(*awaited);
                if (!quiescent && (!awaited || !same(*awaited, quiescent.error())))
                    record(PairwiseEvidenceFailure{quiescent.error()});
                if ((!awaited || !same(*awaited, cleanup.error())) &&
                    (quiescent || !same(quiescent.error(), cleanup.error())))
                    record(PairwiseEvidenceFailure{cleanup.error()});
            }
            // These waits can return the same recorded ordinary worker failure;
            // that is one occurrence, not a second cleanup operation.
            if (cleanup && !quiescent) {
                auto failure = PairwiseEvidenceFailure{std::move(quiescent.error())};
                const auto already_projected = std::ranges::any_of(cleanup->workers(), [&](const auto& worker) {
                        return (worker.failure && same(failure, *worker.failure)) ||
                            (worker.operation_primary && same(failure, *worker.operation_primary));
                    });
                if ((!awaited || !same(failure, *awaited)) && !already_projected)
                    record(std::move(failure));
            }
            if (!primary) primary.emplace(transaction_failure(PairwiseFailureReason::contained_exception));
            if (auto aborted = monitoring_->abort_capture(std::move(*capture)); !aborted)
                append_capture(std::move(aborted.error()));
            return observation_failure<FactualObservationEvidence>(std::move(*primary), block);
        };
        if (!timed) {
            const auto failure = PairwiseEvidenceFailure{std::move(timed.error())};
            return failed_completion(std::nullopt, &failure);
        }
        auto controller_end = clock(PairwiseTypedClockSite::controller_end, first);
        if (!controller_end)
            return failed_completion(PairwiseEvidenceFailure{std::move(controller_end.error())}, nullptr);
        auto enclosing = interval(*controller_start, *controller_end);
        if (!enclosing)
            return failed_completion(std::move(enclosing.error().primary), nullptr);
        const auto injected_fault = worker_dependencies_.hooks.observation_fault;
        if (injected_fault == PairwiseTypedObservationFault::interval) {
            auto injected = PairwiseMeasurementLeafFacade{}.measurement_interval(
                platform_linux::RawMonotonicTimestamp::from_nanoseconds(1U),
                platform_linux::RawMonotonicTimestamp::from_nanoseconds(0U),
                PairwiseVerificationPhase::before);
            if (injected) std::terminate();
            return failed_completion(PairwiseEvidenceFailure{std::move(injected.error())}, nullptr);
        }
        auto quiescent = generation->await_quiescence();
        auto outcome = generation->collect_ordered_results();
        std::optional<PairwiseEvidenceFailure> primary;
        const auto record = [&](PairwiseEvidenceFailure value) {
            if (!primary) primary.emplace(std::move(value));
            else append_secondary(std::move(value));
        };
        if (outcome) {
            for (const auto& worker : outcome->workers()) {
                if (worker.operation_primary) record(*worker.operation_primary);
                else if (worker.failure) record(*worker.failure);
                if (worker.operation_cleanup) append_secondary(*worker.operation_cleanup);
            }
        } else {
            if (!quiescent) record(PairwiseEvidenceFailure{std::move(quiescent.error())});
            record(PairwiseEvidenceFailure{std::move(outcome.error())});
        }
        if (primary) {
            if (auto aborted = monitoring_->abort_capture(std::move(*capture)); !aborted)
                append_capture(std::move(aborted.error()));
            return observation_failure<FactualObservationEvidence>(std::move(*primary), block);
        }
        const auto fail_before_completion = [&](PairwiseEvidenceFailure value)
            -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
            if (auto aborted = monitoring_->abort_capture(std::move(*capture)); !aborted)
                append_capture(std::move(aborted.error()));
            return observation_failure<FactualObservationEvidence>(std::move(value), block);
        };
        for (std::size_t index{}; index < owners.size(); ++index) {
            auto measured = interval(*owners[index].last_start,
                                     *owners[index].last_end);
            if (!measured) {
                auto failed = std::move(measured.error());
                if (auto aborted = monitoring_->abort_capture(std::move(*capture)); !aborted)
                    append_capture(std::move(aborted.error()));
                return observation_failure<FactualObservationEvidence>(
                    std::move(failed.primary), block);
            }
            if constexpr (std::is_same_v<Work,
                          kernel::SequentialReadBandwidthPassCount>) {
                if (!destination.append_bandwidth_worker(
                    measurement::SequentialReadBandwidthWorkerObservation::create(
                        worker_specs_[index].worker_id, std::move(*measured),
                        *owners[index].last_bandwidth_work,
                        *owners[index].last_bandwidth_verification)))
                    std::terminate();
            } else {
                if (!destination.append_latency_worker(
                    measurement::DependentLoadLatencyWorkerObservation::create(
                        worker_specs_[index].worker_id, std::move(*measured),
                        *owners[index].last_latency_schedule,
                        *owners[index].last_latency_verification)))
                    std::terminate();
            }
        }
        PairwiseMeasurementLeafFacade measurement;
        const auto finish = [&](auto observation)
            -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
            auto factual_monitoring = monitoring_->complete_capture(
                std::move(*capture), observation.enclosing_interval().start(),
                observation.enclosing_interval().end(), platform_);
            if (!factual_monitoring) {
                auto cursor = PairwiseMonitoringCaptureFailureCursor::consume(
                    std::move(factual_monitoring.error()));
                auto failed_primary = cursor.next();
                while (!cursor.empty()) append_secondary(cursor.next());
                return observation_failure<FactualObservationEvidence>(
                    std::move(failed_primary), block);
            }
            for (std::size_t index{}; index < owners.size(); ++index) {
                const auto qualification = owners[index].owner->region().qualification_state();
                const auto placement = qualification == memory::PlacementQualificationState::skipped
                    ? measurement::WorkerPlacementValidityState::not_applicable
                    : qualification == memory::PlacementQualificationState::qualified
                        ? measurement::WorkerPlacementValidityState::qualified
                        : measurement::WorkerPlacementValidityState::not_qualified;
                if (!destination.append_placement(
                        measurement::WorkerPlacementValidityEvidence::create(
                            worker_specs_[index].worker_id, placement)) ||
                    !destination.append_migration(
                        measurement::WorkerMigrationValidityEvidence::create(
                            worker_specs_[index].worker_id,
                            outcome->workers()[index].migrated
                                ? measurement::WorkerMigrationValidityState::migrated
                                : measurement::WorkerMigrationValidityState::not_migrated)))
                    std::terminate();
            }
            auto placements = destination.take_placements();
            auto migrations = destination.take_migrations();
            auto captured = std::move(*factual_monitoring).take_result();
            if (!captured) std::terminate();
            const auto monitoring = monitoring_evidence(*captured);
            const auto validity_placements =
                injected_fault == PairwiseTypedObservationFault::validity
                    ? std::span<const measurement::WorkerPlacementValidityEvidence>{}
                    : std::span<const measurement::WorkerPlacementValidityEvidence>{placements};
            auto validity = [&]() {
                if constexpr (std::is_same_v<decltype(observation),
                              measurement::SequentialReadBandwidthObservation>)
                    return measurement.bandwidth_validity(
                        observation, validity_,
                        action.selected_state().timer_overhead_estimate(),
                        validity_placements, migrations, monitoring);
                else
                    return measurement.latency_validity(
                        observation, validity_,
                        action.selected_state().timer_overhead_estimate(),
                        validity_placements, migrations, monitoring);
            }();
            if (!validity)
                return observation_failure<FactualObservationEvidence>(
                    PairwiseEvidenceFailure{std::move(validity.error())}, block);
            return FactualObservationEvidence{
                action.slot(), action.observation_id(), std::move(observation),
                std::move(*validity), std::move(*captured),
                std::move(placements), std::move(migrations)};
        };
        if constexpr (std::is_same_v<Work,
                      kernel::SequentialReadBandwidthPassCount>) {
            if (injected_fault == PairwiseTypedObservationFault::worker_validation) {
                auto injected = measurement.validate_bandwidth_workers(
                    {}, destination.take_bandwidth_workers());
                if (injected) std::terminate();
                return fail_before_completion(
                    PairwiseEvidenceFailure{std::move(injected.error())});
            }
            auto validated = measurement.validate_bandwidth_workers(
                worker_ids_, destination.take_bandwidth_workers());
            if (!validated)
                return fail_before_completion(
                    PairwiseEvidenceFailure{std::move(validated.error())});
            auto observation = measurement.bandwidth_observation(
                action.observation_id(), action.experiment_id(), block,
                injected_fault == PairwiseTypedObservationFault::observation
                    ? kernel::KernelImplementationId::scalar_u64_random_cycle_v1
                    : implementation_id_,
                *bandwidth_configuration_, work,
                std::move(*validated), std::move(*enclosing));
            if (!observation)
                return fail_before_completion(
                    PairwiseEvidenceFailure{std::move(observation.error())});
            return finish(std::move(*observation));
        } else {
            if (injected_fault == PairwiseTypedObservationFault::worker_validation) {
                auto injected = measurement.validate_latency_workers(
                    {}, destination.take_latency_workers());
                if (injected) std::terminate();
                return fail_before_completion(
                    PairwiseEvidenceFailure{std::move(injected.error())});
            }
            auto validated = measurement.validate_latency_workers(
                worker_ids_, destination.take_latency_workers());
            if (!validated)
                return fail_before_completion(
                    PairwiseEvidenceFailure{std::move(validated.error())});
            auto observation = measurement.latency_observation(
                action.observation_id(), action.experiment_id(), block,
                injected_fault == PairwiseTypedObservationFault::observation
                    ? kernel::KernelImplementationId::scalar_u64_sequential_v1
                    : implementation_id_,
                *latency_configuration_,
                std::move(*validated), std::move(*enclosing));
            if (!observation)
                return fail_before_completion(
                    PairwiseEvidenceFailure{std::move(observation.error())});
            return finish(std::move(*observation));
        }
    }

    [[nodiscard]] auto materialize_kernels() noexcept
        -> std::expected<void, PairwiseEvidenceFailure> {
        if (!all_owned(first_owners_) || !all_owned(second_owners_) ||
            first_owners_.size() != second_owners_.size())
            return std::unexpected{
                transaction_failure(PairwiseFailureReason::contained_exception)};
        const auto materialize_bandwidth = [&](auto& destinations)
            -> std::expected<void, PairwiseEvidenceFailure> {
            if (!bandwidth_configuration_)
                return std::unexpected{
                    transaction_failure(PairwiseFailureReason::contained_exception)};
            for (auto& destination : destinations) {
                auto input = kernel_.bandwidth_input(
                    *bandwidth_configuration_, destination.owner->requested_region());
                if (!input)
                    return std::unexpected{
                        PairwiseEvidenceFailure{std::move(input.error())}};
                destination.bandwidth.emplace(
                    kernel::PreparedSequentialReadBandwidth::prepare(std::move(*input)));
                if (!bind_kernel_module(*destination.bandwidth, implementation_id_))
                    return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
            }
            return {};
        };
        const auto materialize_latency = [&](auto& destinations)
            -> std::expected<void, PairwiseEvidenceFailure> {
            if (!latency_configuration_ || !latency_verification_)
                return std::unexpected{
                    transaction_failure(PairwiseFailureReason::contained_exception)};
            for (auto& destination : destinations) {
                auto input = kernel_.latency_input(
                    *latency_configuration_, destination.owner->requested_region());
                if (!input)
                    return std::unexpected{
                        PairwiseEvidenceFailure{std::move(input.error())}};
                destination.latency.emplace(
                    kernel::PreparedDependentLoadLatency::prepare(std::move(*input)));
                if (!bind_kernel_module(*destination.latency, implementation_id_))
                    return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
            }
            return {};
        };
        if (family_ == PairwiseTypedPreparedFamily::bandwidth) {
            if (auto first = materialize_bandwidth(first_owners_); !first)
                return first;
            return materialize_bandwidth(second_owners_);
        }
        if (auto first = materialize_latency(first_owners_); !first)
            return first;
        return materialize_latency(second_owners_);
    }

    static auto
    all_owned(const std::vector<PairwisePreparedMemoryOwnerDestination>& values) noexcept -> bool {
        for (const auto& value : values)
            if (!value.owner)
                return false;
        return true;
    }

    [[nodiscard]] auto acquire_team(PairwiseTypedFailureSink& sink) noexcept
        -> std::expected<void, PairwiseEvidenceFailure> {
        if (team_)
            return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
        auto result = PairwiseWorkerOwningFacade::start(std::move(prepared_team_),
                                                        std::move(worker_dependencies_));
        if (result) {
            team_.emplace(std::move(*result));
            if (preparation_graph_ && !preparation_graph_->capture_workers(team_->readiness())) {
                retire_team(sink);
                return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
            }
            return {};
        }
        auto failure = std::move(result.error());
        for (const auto& value : failure.cleanup)
            static_cast<void>(sink.append(PairwiseEvidenceFailure{value}));
        return std::unexpected{PairwiseEvidenceFailure{std::move(failure.primary)}};
    }

    [[nodiscard]] auto acquire_memory(std::vector<PairwisePreparedMemoryInput>& inputs,
                                      std::vector<PairwisePreparedMemoryOwnerDestination>& owners,
                                      PairwiseTypedFailureSink& sink) noexcept
        -> std::expected<void, PairwiseEvidenceFailure> {
        if (!team_ || inputs.size() != owners.size())
            return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
        struct Context final {
            PairwiseTypedOwnerTransaction* transaction;
            std::vector<PairwisePreparedMemoryInput>* inputs;
            std::vector<PairwisePreparedMemoryOwnerDestination>* owners;
        } context{this, &inputs, &owners};
        const auto operation =
            [](void* opaque, const WorkerExecutionContext& worker,
               const PairwiseWorkerExecutionControl& control) noexcept
            -> PairwiseWorkerCallbackResult {
            struct TimedCompletion final {
                const PairwiseWorkerExecutionControl& control;
                ~TimedCompletion() noexcept { control.signal_timed_completion(); }
            } timed_completion{control};
            auto& state = *static_cast<Context*>(opaque);
            const auto index = worker.worker_index;
            if (index >= state.inputs->size() || index >= state.owners->size())
                std::terminate();
            auto& input = (*state.inputs)[index];
            auto& destination = (*state.owners)[index];
            PairwiseMemoryOwningFacade memory{state.transaction->platform_,
                                              state.transaction->kernel_};
            const auto fail = [&destination](PairwiseEvidenceFailure primary,
                                 std::optional<PairwiseEvidenceFailure> cleanup =
                                     std::nullopt) -> PairwiseWorkerCallbackResult {
                auto typed_primary = PairwiseWorkerOperationFailure::create(std::move(primary));
                if (!typed_primary)
                    std::terminate();
                if (cleanup) {
                    destination.deferred_cleanup.emplace(std::move(*cleanup));
                }
                return std::unexpected{PairwiseWorkerCallbackFailure{std::move(*typed_primary),
                                                                     std::nullopt}};
            };
            auto mapped = memory.map(std::move(input.request));
            if (!mapped) {
                auto failure = std::move(mapped.error());
                std::optional<PairwiseEvidenceFailure> cleanup;
                if (failure.cleanup)
                    cleanup.emplace(std::move(*failure.cleanup));
                return fail(PairwiseEvidenceFailure{std::move(failure.primary)},
                            std::move(cleanup));
            }
            auto bound = memory.bind(std::move(*mapped), input.workspace);
            if (!bound) {
                auto failure = std::move(bound.error());
                auto primary = PairwiseEvidenceFailure{std::move(failure.primary)};
                auto cleaned = memory.cleanup(std::move(failure.owner));
                std::optional<PairwiseEvidenceFailure> cleanup;
                if (!cleaned)
                    cleanup.emplace(std::move(cleaned.error().primary));
                return fail(std::move(primary), std::move(cleanup));
            }
            auto initialized =
                memory.initialize(std::move(*bound), &state.transaction->initializer_);
            if (!initialized) {
                auto failure = std::move(initialized.error());
                auto primary = std::visit(
                    [](auto&& value) -> PairwiseEvidenceFailure {
                        return PairwiseEvidenceFailure{std::move(value)};
                    },
                    std::move(failure.primary));
                auto cleaned = memory.cleanup(std::move(failure.owner));
                std::optional<PairwiseEvidenceFailure> cleanup;
                if (!cleaned)
                    cleanup.emplace(std::move(cleaned.error().primary));
                return fail(std::move(primary), std::move(cleanup));
            }
            auto observed = memory.observe(std::move(*initialized), input.workspace);
            if (!observed) {
                auto failure = std::move(observed.error());
                auto primary = PairwiseEvidenceFailure{std::move(failure.primary)};
                auto cleaned = memory.cleanup(std::move(failure.owner));
                std::optional<PairwiseEvidenceFailure> cleanup;
                if (!cleaned)
                    cleanup.emplace(std::move(cleaned.error().primary));
                return fail(std::move(primary), std::move(cleanup));
            }
            auto evaluated = memory.evaluate(std::move(*observed));
            if (!evaluated) {
                auto failure = std::move(evaluated.error());
                auto primary = PairwiseEvidenceFailure{std::move(failure.primary)};
                auto cleaned = memory.cleanup(std::move(failure.owner));
                std::optional<PairwiseEvidenceFailure> cleanup;
                if (!cleaned)
                    cleanup.emplace(std::move(cleaned.error().primary));
                return fail(std::move(primary), std::move(cleanup));
            }
            auto protected_region = memory.protect(std::move(*evaluated));
            if (!protected_region) {
                auto failure = std::move(protected_region.error());
                const auto reason =
                    failure.primary.reason() ==
                            PairwiseFailureReason::execution_not_eligible
                        ? memory::UnprotectedFinalizationReason::execution_ineligible
                        : memory::UnprotectedFinalizationReason::protection_failed;
                auto finalized = memory.finalize_unprotected(
                    std::move(failure.owner), reason);
                if (!finalized) {
                    auto projected =
                        project_pairwise_memory_finalization_failure(std::move(finalized.error()));
                    destination.deferred_cleanup_tail = std::move(projected.cleanup);
                    return fail(PairwiseEvidenceFailure{std::move(failure.primary)},
                                PairwiseEvidenceFailure{
                                    std::move(projected.primary)});
                }
                return fail(PairwiseEvidenceFailure{std::move(failure.primary)});
            }
            destination.owner.emplace(std::move(*protected_region));
            return {};
        };
        if (progress_) progress_->invalidate(this);
        auto generation = team_->begin_generation(PairwiseWorkerGenerationClass::memory_preparation,
                                                  {.context = &context, .run = operation},
                                                  WorkerObservationPolicy::none,
                                                  WorkerCancellationPolicy::cleanup_insensitive);
        if (!generation)
            return std::unexpected{PairwiseEvidenceFailure{std::move(generation.error())}};
        auto ready = generation->await_ready();
        if (!ready)
            return std::unexpected{PairwiseEvidenceFailure{std::move(ready.error())}};
        generation->release();
        auto timed = generation->await_timed_completion();
        auto quiescent = generation->await_quiescence();
        auto outcome = generation->collect_ordered_results();
        if (!outcome)
            return std::unexpected{PairwiseEvidenceFailure{std::move(outcome.error())}};
        std::optional<PairwiseEvidenceFailure> primary;
        std::size_t worker_index{};
        for (const auto& worker : outcome->workers()) {
            if (worker.operation_primary) {
                if (!primary)
                    primary.emplace(*worker.operation_primary);
                else
                    static_cast<void>(sink.append(*worker.operation_primary));
                if (worker.operation_cleanup)
                    static_cast<void>(sink.append(*worker.operation_cleanup));
                if (worker_index < owners.size() &&
                    owners[worker_index].deferred_cleanup) {
                    static_cast<void>(sink.append(
                        std::move(*owners[worker_index].deferred_cleanup)));
                    owners[worker_index].deferred_cleanup.reset();
                }
                if (worker_index < owners.size() &&
                    owners[worker_index].deferred_cleanup_tail) {
                    static_cast<void>(sink.append(
                        std::move(*owners[worker_index].deferred_cleanup_tail)));
                    owners[worker_index].deferred_cleanup_tail.reset();
                }
            } else if (worker.failure) {
                if (!primary)
                    primary.emplace(*worker.failure);
                else
                    static_cast<void>(sink.append(PairwiseEvidenceFailure{*worker.failure}));
            }
            ++worker_index;
        }
        if (primary)
            return std::unexpected{std::move(*primary)};
        if (!timed)
            return std::unexpected{PairwiseEvidenceFailure{std::move(timed.error())}};
        if (!quiescent)
            return std::unexpected{PairwiseEvidenceFailure{std::move(quiescent.error())}};
        if (preparation_graph_) {
            const bool second=&owners==&second_owners_;
            for (std::size_t index{};index<owners.size();++index) {
                if (!owners[index].owner) std::terminate();
                const auto& region=*owners[index].owner;
                if (!preparation_graph_->capture_memory(second,index,{region.request(),
                    region.region().region().region().region().binding_evidence(),region.region().region().region().initialization_evidence(),
                    region.region().region().query_state(),region.region().region().observation(),region.region().evaluation(),
                    region.region().qualification_state(),region.protection_evidence()})) {
                    retire_memory(inputs,owners,sink);
                    return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
                }
            }
            if (!preparation_graph_->transition(second?PreparationGraphStage::second_memory:PreparationGraphStage::first_memory)) {
                retire_memory(inputs,owners,sink);
                return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
            }
        }
        return {};
    }

    [[nodiscard]] auto acquire_monitoring(PairwiseTypedFailureSink& sink) noexcept
        -> std::expected<void, PairwiseEvidenceFailure> {
        if (!team_ || monitoring_)
            return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
        auto capability = team_->lend_monitoring_readiness();
        if (!capability)
            return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
        auto bound = std::move(prepared_monitoring_).bind_worker_readiness(std::move(*capability));
        if (!bound)
            return monitoring_failure(std::move(bound.error()), sink);
        auto owner = std::move(*bound).acquire_sources_and_owner(platform_);
        if (!owner)
            return monitoring_failure(std::move(owner.error()), sink);
        monitoring_.emplace(std::move(*owner));
        if (preparation_graph_) {
            const auto view=monitoring_->preparation_graph_facts();
            if (!view || !preparation_graph_->capture_monitoring(*view)) {
                retire_monitoring(sink);
                return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
            }
        }
        return {};
    }

    static auto monitoring_failure(PairwiseMonitoringPreparationFailure failure,
                                   PairwiseTypedFailureSink& sink) noexcept
        -> std::expected<void, PairwiseEvidenceFailure> {
        auto cursor = PairwiseMonitoringFailureCursor::consume(std::move(failure));
        if (cursor.empty())
            return std::unexpected{transaction_failure(PairwiseFailureReason::contained_exception)};
        auto primary = cursor.next();
        while (!cursor.empty())
            static_cast<void>(sink.append(cursor.next()));
        return std::unexpected{std::move(primary)};
    }

    void retire_memory(std::vector<PairwisePreparedMemoryInput>& inputs,
                       std::vector<PairwisePreparedMemoryOwnerDestination>& owners,
                       PairwiseTypedFailureSink& sink) noexcept {
        PairwiseMemoryOwningFacade memory{platform_, kernel_};
        for (std::size_t index{}; index < owners.size(); ++index) {
            if (!owners[index].owner)
                continue;
            owners[index].bandwidth.reset();
            owners[index].latency.reset();
            auto result = memory.finalize(std::move(*owners[index].owner),
                                          memory::PostRunVerificationApplicability::not_applicable,
                                          inputs[index].workspace);
            owners[index].owner.reset();
            if (!result) {
                auto projected =
                    project_pairwise_memory_finalization_failure(std::move(result.error()));
                static_cast<void>(
                    sink.append(PairwiseEvidenceFailure{std::move(projected.primary)}));
                if (projected.cleanup)
                    static_cast<void>(
                        sink.append(PairwiseEvidenceFailure{std::move(*projected.cleanup)}));
            }
        }
    }

    void retire_monitoring(PairwiseTypedFailureSink& sink) noexcept {
        if (!monitoring_)
            return;
        auto cursor =
            PairwiseMonitoringFailureCursor::consume(monitoring_->stop_and_retire(platform_));
        monitoring_.reset();
        while (!cursor.empty())
            static_cast<void>(sink.append(cursor.next()));
    }

    void retire_team(PairwiseTypedFailureSink& sink) noexcept {
        if (!team_)
            return;
        auto result = team_->stop_and_join();
        if (!result) {
            static_cast<void>(
                sink.append(PairwiseEvidenceFailure{std::move(result.error().primary)}));
            for (auto& value : result.error().cleanup)
                static_cast<void>(sink.append(PairwiseEvidenceFailure{std::move(value)}));
        }
        team_.reset();
    }

    void retire_post_transfer() noexcept {
        if (!transferred_)
            return;
        // The selected post-transfer binding will replace this emergency-only
        // retirement with its reportable finalization path. No generic owner
        // callback may reverse this order after transfer.
        auto ignore = [](PairwiseEvidenceFailure) noexcept {};
        PairwiseMemoryOwningFacade memory{platform_, kernel_};
        const auto retire = [&](auto& inputs, auto& owners) noexcept {
            for (std::size_t index{}; index < owners.size(); ++index) {
                if (!owners[index].owner)
                    continue;
                owners[index].bandwidth.reset();
                owners[index].latency.reset();
                auto result =
                    memory.finalize(std::move(*owners[index].owner),
                                    memory::PostRunVerificationApplicability::not_applicable,
                                    inputs[index].workspace);
                owners[index].owner.reset();
                if (!result) {
                    auto projected =
                        project_pairwise_memory_finalization_failure(std::move(result.error()));
                    ignore(PairwiseEvidenceFailure{std::move(projected.primary)});
                }
            }
        };
        retire(first_inputs_, first_owners_);
        retire(second_inputs_, second_owners_);
        if (monitoring_) {
            static_cast<void>(monitoring_->stop_and_retire(platform_));
            monitoring_.reset();
        }
        if (team_) {
            static_cast<void>(team_->stop_and_join());
            team_.reset();
        }
        transferred_ = false;
    }

    PairwiseProgressState* progress_{};
    std::unique_ptr<RunnerCancellation> cancellation_;
    std::shared_ptr<RunnerCancellation> shared_cancellation_;
    std::vector<WorkerLaunchSpec> worker_specs_;
    std::vector<core::WorkerId> worker_ids_;
    measurement::CalibrationDurationPolicy calibration_;
    measurement::WarmupPolicy warmup_;
    measurement::RepetitionPolicy repetition_;
    measurement::ComparisonOrderPolicy comparison_order_;
    measurement::ObservationValidityPolicy validity_;
    [[maybe_unused]] bool calibration_enabled_{};
    PairwisePreparedTypedMeasurement measurement_;
    PairwisePreparedWorkerTeam prepared_team_;
    PairwisePreparedMonitoringStorage prepared_monitoring_;
    std::vector<PairwisePreparedMemoryInput> first_inputs_;
    std::vector<PairwisePreparedMemoryInput> second_inputs_;
    std::vector<PairwisePreparedMemoryOwnerDestination> first_owners_;
    std::vector<PairwisePreparedMemoryOwnerDestination> second_owners_;
    PairwisePreparedRegionInitializer initializer_;
    PairwisePlatformLeafFacade platform_;
    PairwiseKernelLeafFacade kernel_;
    PairwiseWorkerOwningDependencies worker_dependencies_;
    std::optional<PairwisePersistentWorkerTeam> team_;
    std::optional<PairwiseMonitoringOwningFacade> monitoring_;
    ipc::RequestId first_request_;
    ipc::RequestId second_request_;
    AdmittedPlacementIdentity first_identity_;
    AdmittedPlacementIdentity second_identity_;
    [[maybe_unused]] PairwiseTypedPreparedFamily family_;
    [[maybe_unused]] std::uint64_t comparison_order_seed_{};
    monitoring::MonitoringExecutionPolicy monitoring_policy_;
    ServicePlacementPlan service_placement_;
    StatisticsProductionPolicy statistics_policy_;
    [[maybe_unused]] std::size_t maximum_blocks_{};
    std::optional<kernel::SequentialReadBandwidthConfig> bandwidth_configuration_;
    std::optional<kernel::DependentLoadLatencyConfig> latency_configuration_;
    std::optional<kernel::PreparedDependentLoadLatencyVerification>
        latency_verification_;
    kernel::KernelImplementationId implementation_id_{};
    [[maybe_unused]] std::uint64_t resolved_latency_seed_{};
    std::uint64_t measurement_root_seed_{};
    core::ExperimentId calibration_experiment_id_{0U};
    kernel::DependentLoadLatencyBlockId calibration_block_id_{0U};
    std::vector<PairwiseEvidenceFailure> operation_secondary_;
    std::vector<PairwisePreparedFinalizationSlot> first_finalization_slots_;
    std::vector<PairwisePreparedFinalizationSlot> second_finalization_slots_;
    std::vector<FinalizedWorkerEvidence> first_finalized_workers_;
    std::vector<FinalizedWorkerEvidence> second_finalized_workers_;
    std::vector<LocalFinalizationErrorAssociation> finalization_associations_;
    std::optional<PairwisePreparationGraph> preparation_graph_;
    bool transferred_{};
    PairwiseTypedPreparedOwners* bound_owners_{};
    friend struct PairwiseTypedOwnerTransactionTestAccess;
};

auto acquire_owner(void* opaque, PairwiseTypedOwnerStage stage,
                   PairwiseTypedFailureSink& sink) noexcept
    -> std::expected<void, PairwiseEvidenceFailure> {
    return static_cast<PairwiseTypedOwnerTransaction*>(opaque)->acquire(stage, sink);
}
void cleanup_owner(void* opaque, PairwiseTypedOwnerStage stage,
                   PairwiseTypedFailureSink& sink) noexcept {
    static_cast<PairwiseTypedOwnerTransaction*>(opaque)->cleanup(stage, sink);
}
bool admit_observation_owner(
    void* opaque, const measurement::ScheduledObservationSlot& slot) noexcept {
    return static_cast<const PairwiseTypedOwnerTransaction*>(opaque)
        ->admit_observation(slot);
}
void bind_prepared_owners(void* opaque,
                          PairwiseTypedPreparedOwners* owners) noexcept {
    static_cast<PairwiseTypedOwnerTransaction*>(opaque)
        ->bind_prepared_owners(owners);
}

auto prepare_pairwise_typed_owner_transaction_storage(PairwiseTypedPreOwnerPreparation&& prepared,
                                                     PairwiseProgressState* progress) noexcept
    -> PairwiseOperationResult<PairwiseTypedPreparedOwners> {
    if (!prepared.active_)
        return std::unexpected{PairwiseOperationFailure{
            transaction_failure(PairwiseFailureReason::contained_exception), {}}};
    auto adapter = std::move(prepared.adapter_);
    adapter.install_observation_workers(std::move(prepared.observation_workers_));
    std::unique_ptr<PairwiseTypedOwnerTransaction> context;
    try {
        context = std::make_unique<PairwiseTypedOwnerTransaction>(std::move(prepared), progress);
    } catch (...) {
        auto failed =
            prepare_pairwise_typed_owners(std::move(adapter), PairwiseTypedOwnerOperations{});
        auto released = std::move(failed.error()).release();
        return std::unexpected{
            materialize_pairwise_owner_preparation_failure(std::move(released))};
    }
    auto result = prepare_pairwise_typed_owner_storage(
        std::move(adapter),
        PairwiseTypedOwnerOperations{PairwiseTypedOwnedContext::from_unique(std::move(context)),
                                     acquire_owner, cleanup_owner,
                                     admit_observation_owner,
                                     bind_prepared_owners});
    if (!result) {
        auto released = std::move(result.error()).release();
        return std::unexpected{
            materialize_pairwise_owner_preparation_failure(std::move(released))};
    }
    return std::move(*result);
}

auto prepare_pairwise_typed_owner_transaction(PairwiseTypedPreOwnerPreparation&& prepared) noexcept
    -> PairwiseOperationResult<PairwiseTypedPreparedOwners> {
    auto pending = prepare_pairwise_typed_owner_transaction_storage(std::move(prepared));
    if (!pending) return std::unexpected{std::move(pending.error())};
    auto acquired = acquire_pairwise_typed_owners(std::move(*pending));
    if (!acquired)
        return std::unexpected{materialize_pairwise_owner_preparation_failure(
            std::move(acquired.error()).release())};
    return std::move(*acquired);
}

auto PairwiseTypedOwnerTransactionTestAccess::state(
    const PairwiseTypedPreparedOwners& owners) noexcept -> State {
    const auto& transaction =
        *static_cast<const PairwiseTypedOwnerTransaction*>(owners.context());
    const auto count = [](const auto& values) {
        std::size_t result{};
        for (const auto& value : values)
            result += value.owner.has_value() ? 1U : 0U;
        return result;
    };
    const auto verification_storage = transaction.latency_verification_
        ? kernel::detail::KernelLeafAccess::verification_storage(
              *transaction.latency_verification_)
        : std::array<const void*, 2U>{};
    const auto verification_capacities = transaction.latency_verification_
        ? kernel::detail::KernelLeafAccess::verification_capacities(
              *transaction.latency_verification_)
        : std::array<std::size_t, 2U>{};
    const auto observation_facts = std::pair{
        owners.storage().factual_destination_count(),
        owners.storage().factual_worker_capacity()};
    return {.transferred = transaction.transferred_,
            .team = transaction.team_.has_value(),
            .monitoring = transaction.monitoring_.has_value(),
            .first_owners = count(transaction.first_owners_),
            .second_owners = count(transaction.second_owners_),
            .first_owner_storage = transaction.first_owners_.data(),
            .second_owner_storage = transaction.second_owners_.data(),
            .measurement_root_seed = transaction.measurement_root_seed_,
            .calibration_experiment_id =
                transaction.calibration_experiment_id_.value(),
            .calibration_block_id = transaction.calibration_block_id_.value(),
            .latency_verification_storage = verification_storage,
            .latency_verification_capacities = verification_capacities,
            .operation_secondary_storage = transaction.operation_secondary_.data(),
            .operation_secondary_capacity = transaction.operation_secondary_.capacity(),
            .finalization_storage = {
                transaction.first_finalization_slots_.data(),
                transaction.second_finalization_slots_.data(),
                transaction.first_finalized_workers_.data(),
                transaction.second_finalized_workers_.data(),
                transaction.finalization_associations_.data()},
            .finalization_capacity = {
                transaction.first_finalization_slots_.capacity(),
                transaction.second_finalization_slots_.capacity(),
                transaction.first_finalized_workers_.capacity(),
                transaction.second_finalized_workers_.capacity(),
                transaction.finalization_associations_.capacity()},
            .bandwidth_configuration = transaction.bandwidth_configuration_,
            .latency_configuration = transaction.latency_configuration_,
            .implementation_id = transaction.implementation_id_,
            .observation_destination_count = observation_facts.first,
            .observation_worker_capacity = observation_facts.second,
            .bound_to_current_envelope = transaction.bound_owners_ == &owners};
}

auto PairwiseTypedOwnerTransactionTestAccess::command_facts(
    const PairwiseTypedPreparedOwners& owners, bool first,
    std::size_t worker) noexcept -> CommandFacts {
    const auto& transaction =
        *static_cast<const PairwiseTypedOwnerTransaction*>(owners.context());
    const auto& destinations = first ? transaction.first_owners_
                                     : transaction.second_owners_;
    if (worker >= destinations.size())
        std::terminate();
    const auto& value = destinations[worker];
    return {.prepared = value.bandwidth.has_value() || value.latency.has_value(),
            .started = value.last_start.has_value(),
            .ended = value.last_end.has_value(),
            .work_units = value.last_work_units,
            .logical_bytes = value.last_logical_bytes,
            .expected_terminal = value.last_expected_terminal,
            .actual_terminal = value.last_actual_terminal,
            .verification_matched = value.last_verification_matched};
}

auto PairwiseTypedOwnerTransactionTestAccess::observation_worker_storage(
    const PairwiseTypedPreparedOwners& owners, std::size_t index) noexcept
    -> const void* {
    return PairwiseTypedAdapterStorageTestAccess::observation_worker_storage(
        owners.storage(), index);
}

auto PairwiseTypedOwnerTransactionTestAccess::observation_worker_capacity(
    const PairwiseTypedPreparedOwners& owners, std::size_t index) noexcept
    -> std::size_t {
    return PairwiseTypedAdapterStorageTestAccess::observation_worker_capacity(
        owners.storage(), index);
}
auto PairwiseTypedOwnerTransactionTestAccess::admit_observation(
    const PairwiseTypedPreparedOwners& owners,
    const measurement::ScheduledObservationSlot& slot) noexcept -> bool {
    return static_cast<const PairwiseTypedOwnerTransaction*>(owners.context())
        ->admit_observation(slot);
}

auto PairwiseTypedOwnerTransactionTestAccess::calibrate_bandwidth(
    PairwiseTypedPreparedOwners& owners) noexcept
    -> PairwiseTypedOperationSourceResult<
        measurement::SequentialReadBandwidthCalibrationOutcome> {
    return static_cast<PairwiseTypedOwnerTransaction*>(owners.context())
        ->calibrate_bandwidth();
}

auto PairwiseTypedOwnerTransactionTestAccess::calibrate_latency(
    PairwiseTypedPreparedOwners& owners) noexcept
    -> PairwiseTypedOperationSourceResult<
        measurement::DependentLoadLatencyCalibrationOutcome> {
    return static_cast<PairwiseTypedOwnerTransaction*>(owners.context())
        ->calibrate_latency();
}

auto PairwiseTypedOwnerTransactionTestAccess::warmup_bandwidth(
    PairwiseTypedPreparedOwners& owners,
    kernel::SequentialReadBandwidthPassCount work) noexcept
    -> PairwiseTypedOperationSourceResult<
        measurement::SequentialReadBandwidthWarmupOutcome> {
    return static_cast<PairwiseTypedOwnerTransaction*>(owners.context())
        ->warmup_bandwidth(work);
}

auto PairwiseTypedOwnerTransactionTestAccess::warmup_latency(
    PairwiseTypedPreparedOwners& owners,
    kernel::DependentLoadLatencyStepCount work) noexcept
    -> PairwiseTypedOperationSourceResult<
        measurement::DependentLoadLatencyWarmupOutcome> {
    return static_cast<PairwiseTypedOwnerTransaction*>(owners.context())
        ->warmup_latency(work);
}

namespace {
template <class Action> struct ObservationInvocation final {
    PairwiseTypedOwnerTransaction* transaction{};
    const Action* action{};
    std::optional<FactualObservationEvidence> value;
    std::optional<PairwiseTypedOperationFailureSource> failure;
};

template <class Action>
void invoke_observation(void* opaque,
                        PairwiseTypedObservationDestination& destination) noexcept {
    auto& invocation = *static_cast<ObservationInvocation<Action>*>(opaque);
    auto result = [&]() {
        if constexpr (std::is_same_v<
                          Action,
                          measurement::SequentialReadBandwidthObservationRequiredAction>)
            return invocation.transaction->observe_bandwidth(*invocation.action,
                                                             destination);
        else
            return invocation.transaction->observe_latency(*invocation.action,
                                                           destination);
    }();
    if (result) {
        invocation.value.emplace(std::move(*result));
        return;
    }
    auto error = std::move(result.error());
    if (!destination.set_failed_primary(error.primary)) std::terminate();
    for (const auto& secondary : error.secondary)
        if (!destination.append_failed_secondary(secondary)) std::terminate();
    invocation.failure.emplace(std::move(error));
}

template <class Action>
auto invoke_prepared_observation(PairwiseTypedPreparedOwners& owners,
                                 const Action& action) noexcept
    -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
    ObservationInvocation<Action> invocation{
        static_cast<PairwiseTypedOwnerTransaction*>(owners.context()), &action,
        std::nullopt, std::nullopt};
    if (!owners.consume_observation_destination(
            action.slot(), &invocation, invoke_observation<Action>)) {
        auto failure = transaction_failure(PairwiseFailureReason::contained_exception);
        return std::unexpected{PairwiseTypedOperationFailureSource{
            std::move(failure), {}, action.block_plan().block_id()}};
    }
    if (invocation.failure)
        return std::unexpected{std::move(*invocation.failure)};
    if (!invocation.value) std::terminate();
    return std::move(*invocation.value);
}
} // namespace

auto PairwiseTypedOwnerTransactionTestAccess::observe_bandwidth(
    PairwiseTypedPreparedOwners& owners,
    const measurement::SequentialReadBandwidthObservationRequiredAction& action)
    noexcept -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
    return invoke_prepared_observation(owners, action);
}

auto PairwiseTypedOwnerTransactionTestAccess::observe_latency(
    PairwiseTypedPreparedOwners& owners,
    const measurement::DependentLoadLatencyObservationRequiredAction& action)
    noexcept -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
    return invoke_prepared_observation(owners, action);
}

auto PairwiseTypedOwnerTransactionTestAccess::observe_bandwidth_once(
    PairwiseTypedPreparedOwners& owners,
    kernel::SequentialReadBandwidthPassCount work,
    measurement::TimerOverheadEstimate overhead) noexcept
    -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
    auto& transaction =
        *static_cast<PairwiseTypedOwnerTransaction*>(owners.context());
    auto threshold = measurement::WarmupStabilityThreshold::create(0U, 1U);
    auto disabled = measurement::WarmupPolicy::create(0U, 0U, 0U, *threshold);
    auto state = PairwiseMeasurementLeafFacade{}.create_bandwidth_repetition(
        transaction.calibration_experiment_id_, transaction.repetition_,
        transaction.comparison_order_, false, *disabled,
        measurement::BandwidthSelectedExecutionState::create(work, overhead));
    if (!state) std::terminate();
    auto action = PairwiseMeasurementLeafFacade{}.next_bandwidth_action(*state);
    return invoke_prepared_observation(
        owners,
        std::get<measurement::SequentialReadBandwidthObservationRequiredAction>(
            action));
}

auto PairwiseTypedOwnerTransactionTestAccess::observe_latency_once(
    PairwiseTypedPreparedOwners& owners,
    kernel::DependentLoadLatencyStepCount work,
    measurement::TimerOverheadEstimate overhead) noexcept
    -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
    auto& transaction =
        *static_cast<PairwiseTypedOwnerTransaction*>(owners.context());
    auto threshold = measurement::WarmupStabilityThreshold::create(0U, 1U);
    auto disabled = measurement::WarmupPolicy::create(0U, 0U, 0U, *threshold);
    auto state = PairwiseMeasurementLeafFacade{}.create_latency_repetition(
        transaction.calibration_experiment_id_, transaction.repetition_,
        transaction.comparison_order_, false, *disabled,
        measurement::LatencySelectedExecutionState::create(work, overhead));
    if (!state) std::terminate();
    auto action = PairwiseMeasurementLeafFacade{}.next_latency_action(*state);
    return invoke_prepared_observation(
        owners,
        std::get<measurement::DependentLoadLatencyObservationRequiredAction>(
            action));
}

namespace {
template <class Work>
auto observe_at(PairwiseTypedPreparedOwners& owners, Work work,
                measurement::TimerOverheadEstimate overhead,
                std::size_t ordinal, core::ExperimentId experiment,
                const measurement::RepetitionPolicy& repetition,
                const measurement::ComparisonOrderPolicy& comparison_order) noexcept
    -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
    auto threshold = measurement::WarmupStabilityThreshold::create(0U, 1U);
    auto disabled = measurement::WarmupPolicy::create(0U, 0U, 0U, *threshold);
    PairwiseMeasurementLeafFacade leaf;
    auto created = [&] {
        if constexpr (std::is_same_v<Work,
                      kernel::SequentialReadBandwidthPassCount>)
            return leaf.create_bandwidth_repetition(
                experiment, repetition, comparison_order, false, *disabled,
                measurement::BandwidthSelectedExecutionState::create(work, overhead));
        else
            return leaf.create_latency_repetition(
                experiment, repetition, comparison_order, false, *disabled,
                measurement::LatencySelectedExecutionState::create(work, overhead));
    }();
    if (!created) std::terminate();
    std::optional state{std::move(*created)};
    for (std::size_t index{};;) {
        auto action = [&] {
            if constexpr (std::is_same_v<Work,
                          kernel::SequentialReadBandwidthPassCount>)
                return leaf.next_bandwidth_action(*state);
            else
                return leaf.next_latency_action(*state);
        }();
        using ObservationAction = std::conditional_t<
            std::is_same_v<Work, kernel::SequentialReadBandwidthPassCount>,
            measurement::SequentialReadBandwidthObservationRequiredAction,
            measurement::DependentLoadLatencyObservationRequiredAction>;
        if (auto* observation_action = std::get_if<ObservationAction>(&action)) {
            auto factual = invoke_prepared_observation(owners, *observation_action);
            if (!factual || index++ == ordinal) return factual;
            auto applied = [&] {
                if constexpr (std::is_same_v<Work,
                              kernel::SequentialReadBandwidthPassCount>)
                    return leaf.apply_bandwidth_observation(
                        std::move(*state),
                        std::get<measurement::SequentialReadBandwidthObservation>(
                            factual->observation), factual->validity);
                else
                    return leaf.apply_latency_observation(
                        std::move(*state),
                        std::get<measurement::DependentLoadLatencyObservation>(
                            factual->observation), factual->validity);
            }();
            if (!applied) std::terminate();
            state.emplace(std::move(*applied));
            continue;
        }
        if (!std::holds_alternative<measurement::PrecisionEvaluationRequiredAction>(
                action))
            std::terminate();
        auto applied = [&] {
            if constexpr (std::is_same_v<Work,
                          kernel::SequentialReadBandwidthPassCount>)
                return leaf.apply_bandwidth_precision(
                    std::move(*state),
                    measurement::PrecisionStoppingDecision::target_not_met);
            else
                return leaf.apply_latency_precision(
                    std::move(*state),
                    measurement::PrecisionStoppingDecision::target_not_met);
        }();
        if (!applied) std::terminate();
        state.emplace(std::move(*applied));
    }
}
} // namespace

auto PairwiseTypedOwnerTransactionTestAccess::observe_bandwidth_at(
    PairwiseTypedPreparedOwners& owners,
    kernel::SequentialReadBandwidthPassCount work,
    measurement::TimerOverheadEstimate overhead, std::size_t ordinal) noexcept
    -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
    auto& transaction =
        *static_cast<PairwiseTypedOwnerTransaction*>(owners.context());
    return observe_at(owners, work, overhead, ordinal,
                      transaction.calibration_experiment_id_,
                      transaction.repetition_, transaction.comparison_order_);
}

auto PairwiseTypedOwnerTransactionTestAccess::observe_latency_at(
    PairwiseTypedPreparedOwners& owners,
    kernel::DependentLoadLatencyStepCount work,
    measurement::TimerOverheadEstimate overhead, std::size_t ordinal) noexcept
    -> PairwiseTypedOperationSourceResult<FactualObservationEvidence> {
    auto& transaction =
        *static_cast<PairwiseTypedOwnerTransaction*>(owners.context());
    return observe_at(owners, work, overhead, ordinal,
                      transaction.calibration_experiment_id_,
                      transaction.repetition_, transaction.comparison_order_);
}

auto PairwiseTypedOwnerTransactionTestAccess::finalize(
    PairwiseTypedPreparedOwners& owners) noexcept
    -> PairwiseFinalizationSource {
    return static_cast<PairwiseTypedOwnerTransaction*>(owners.context())->finalize();
}

auto bind_pairwise_typed_owner_run(
    PairwiseTypedPreparedOwners&& owners) noexcept
    -> PairwiseOperationResult<PairwiseProductionRunCore> {
    PairwiseProductionRunFunctions functions{
        .calibrate_bandwidth =
            [](void* opaque,
               const measurement::CalibrationDurationPolicy& policy) noexcept {
                auto& self = *static_cast<PairwiseTypedOwnerTransaction*>(opaque);
                if (!self.matches(policy)) std::terminate();
                return self.calibrate_bandwidth();
            },
        .calibrate_latency =
            [](void* opaque,
               const measurement::CalibrationDurationPolicy& policy) noexcept {
                auto& self = *static_cast<PairwiseTypedOwnerTransaction*>(opaque);
                if (!self.matches(policy)) std::terminate();
                return self.calibrate_latency();
            },
        .warmup_bandwidth =
            [](void* opaque, const measurement::WarmupPolicy& policy,
               kernel::SequentialReadBandwidthPassCount work) noexcept {
                auto& self = *static_cast<PairwiseTypedOwnerTransaction*>(opaque);
                if (!self.matches(policy)) std::terminate();
                return self.warmup_bandwidth(work);
            },
        .warmup_latency =
            [](void* opaque, const measurement::WarmupPolicy& policy,
               kernel::DependentLoadLatencyStepCount work) noexcept {
                auto& self = *static_cast<PairwiseTypedOwnerTransaction*>(opaque);
                if (!self.matches(policy)) std::terminate();
                return self.warmup_latency(work);
            },
        .observe_bandwidth =
            [](void* opaque,
               const measurement::SequentialReadBandwidthObservationRequiredAction&
                   action) noexcept {
                auto& transaction =
                    *static_cast<PairwiseTypedOwnerTransaction*>(opaque);
                return invoke_prepared_observation(
                    transaction.bound_owners(), action);
            },
        .observe_latency =
            [](void* opaque,
               const measurement::DependentLoadLatencyObservationRequiredAction&
                   action) noexcept {
                auto& transaction =
                    *static_cast<PairwiseTypedOwnerTransaction*>(opaque);
                return invoke_prepared_observation(
                    transaction.bound_owners(), action);
            },
        .finalize = [](void* opaque) noexcept {
            return static_cast<PairwiseTypedOwnerTransaction*>(opaque)->finalize();
        }};
    return prepare_pairwise_production_run(
        std::move(owners), functions);
}

} // namespace epycbench::runtime::detail
