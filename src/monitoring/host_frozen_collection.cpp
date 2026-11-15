#include "monitoring/host_frozen_collection.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/host_frozen_process_control.hpp"
#include "runtime/pairwise_monitoring_capacity.hpp"
#include "monitoring/pairwise_prepared_capture_storage.hpp"
#include "platform_linux/hwmon_temperature_access.hpp"

#include <algorithm>
#include <limits>

namespace epycbench::monitoring::qualification::detail {
namespace {
namespace rt = runtime::detail;
namespace work = platform_linux::host_work;
using Reason = HostFrozenCollectionReason;
constexpr std::uint64_t second = UINT64_C(1000000000);
auto plan(std::size_t comparison, std::size_t block) noexcept -> measurement::MeasurementBlockPlan {
    return measurement::MeasurementBlockPlan::create(
        kernel::DependentLoadLatencyBlockId{comparison * 12U + block + 1U},
        measurement::ComparisonOrderPolicy::create(
            measurement::ComparisonOrderStrategy::randomized_balanced, 0U),
        measurement::BlockPreparationPlan::create(false, false));
}
auto identity(HostFrozenPhase phase, std::size_t ordinal) noexcept -> HostFrozenCellIdentity {
    const auto comparison = phase == HostFrozenPhase::q5 ? ordinal : ordinal / 48U;
    const auto block = phase == HostFrozenPhase::q5 ? 0U : ordinal % 48U / 4U;
    const auto slot = phase == HostFrozenPhase::q5 ? 0U : ordinal % 4U;
    const auto treatment = phase == HostFrozenPhase::q5
        ? measurement::ComparisonVariantSlot::second : plan(comparison, block).observations()[slot].comparison_variant();
    const auto configuration = treatment == measurement::ComparisonVariantSlot::first
        ? HostFrozenConfiguration::c0 : static_cast<HostFrozenConfiguration>(comparison % 4U + 1U);
    return {phase, comparison < 4U ? HostQ0bFamily::bandwidth : HostQ0bFamily::latency,
            configuration, ordinal, comparison, block, slot, treatment};
}
auto valid_dependencies(const HostFrozenCollectionDependencies& value) noexcept -> bool {
    return value.capacity.mint && value.guard.now && value.guard.cancelled &&
        value.empty_clock.now && value.headroom.open_root && value.headroom.open_at &&
        value.headroom.metadata && value.headroom.read && value.headroom.close &&
        value.headroom.guard.now && value.headroom.guard.cancelled;
}
auto sample_guard(const work::GuardOperations& guard, runtime::RunnerCancellation& cancellation,
                  std::uint64_t& latest) noexcept -> std::optional<Reason> {
    const auto cancelled = [&]() noexcept {
        return cancellation.requested() || guard.cancelled(guard.context);
    };
    if (cancelled()) {
        (void)cancellation.try_request(runtime::ipc::CancelReason::controller_shutdown);
        return Reason::cancelled;
    }
    std::uint64_t now{};
    const auto sampled = guard.now(guard.context, &now);
    if (sampled.value != 0 || sampled.error_number != 0 || now < latest)
        return Reason::clock;
    latest = now;
    // A successful injected/native clock call may make cancellation observable.
    // Resample before permitting any subsequent bounded or native action.
    if (cancelled()) {
        (void)cancellation.try_request(runtime::ipc::CancelReason::controller_shutdown);
        return Reason::cancelled;
    }
    return std::nullopt;
}
auto absolute_deadline(std::uint64_t begin, std::uint64_t seconds) noexcept
    -> std::optional<std::uint64_t> {
    if (seconds > (UINT64_MAX - begin) / second) return std::nullopt;
    return begin + seconds * second;
}
} // namespace

auto host_frozen_run_resources(const runtime::AdmittedPlacementRequest& placement,
    const rt::PairwiseAdmittedMonitoringCapacity& capacity) noexcept
    -> std::expected<work::Resources, Reason> {
    using Byte = core::ByteCount;
    namespace mon = monitoring;
    using Slot = rt::PairwiseMonitoringCapacitySlot;
    const auto workers = static_cast<std::uint64_t>(placement.workers().size());
    const auto service = static_cast<std::uint64_t>(capacity.asynchronous_requested());
    if (!workers || workers > 8U || workers != capacity.workers())
        return std::unexpected{Reason::capacity};
    std::uint64_t mapped{}, ordinary{}, stack{}, descriptors = 3U;
    const auto add = [](std::uint64_t& total, std::uint64_t count, std::uint64_t width = 1U) noexcept {
        const auto product = Byte::from_base_units(count).checked_multiply(width);
        if (!product) return false;
        const auto result = Byte::from_base_units(total).checked_add(*product);
        if (!result) return false;
        total = result->value(); return true;
    };
    const auto kernel = capacity.kernel_cpu_count(), entries = capacity.enumeration_capacity();
    const auto present = capacity.present_cpu_count(), members = capacity.maximum_membership_count();
    const auto domains = capacity.maximum_domain_count(), page = capacity.base_page_bytes();
    // Capacity already derives these dimensions from the closed native CPU
    // domain. Reject impossible shapes before evaluating any compound term.
    if (!kernel || kernel > UINT64_C(4294967296) || entries != kernel + 1U ||
        !present || present > kernel || members > present || domains > workers ||
        !page || page > UINT64_C(4294967296) ||
        capacity.perf_record_capacity() > UINT64_MAX / sizeof(platform_linux::PerfContextSwitchRecord) ||
        capacity.round_capacity() > UINT64_MAX - 3U ||
        capacity.capture_slots() > UINT64_MAX / (capacity.round_capacity() + 3U))
        return std::unexpected{Reason::capacity};
    constexpr auto word_bits = std::numeric_limits<unsigned long>::digits;
    std::uint64_t node_domain{};
    for (const auto node : placement.context().topology().machine().present_nodes)
        node_domain = std::max(node_domain, static_cast<std::uint64_t>(node.value()) + 1U);
    if (!node_domain) return std::unexpected{Reason::capacity};
    for (const auto& worker : placement.workers()) {
        const auto& region = worker.request().region();
        const auto bytes = region.bytes().value(), region_page = region.page_size_bytes().value();
        if (!region_page || !bytes || bytes > UINT64_MAX - (region_page - 1U) ||
            !add(mapped, (bytes + region_page - 1U) / region_page, region_page))
            return std::unexpected{Reason::capacity};
        const auto pages = (bytes + region_page - 1U) / region_page;
        if (!add(ordinary, 1U, sizeof(rt::PairwiseMemoryWorkspace)) ||
            !add(ordinary, std::min(pages, UINT64_C(4096)),
                 sizeof(int) + sizeof(platform_linux::PageQueryStatus)) ||
            !add(ordinary, std::min(pages, node_domain), 2U * sizeof(memory::LinuxNodePageCount)) ||
            !add(ordinary, (node_domain + word_bits - 1U) / word_bits, 2U * sizeof(unsigned long)))
            return std::unexpected{Reason::capacity};
    }
    if (!add(stack, workers + service, UINT64_C(8388608)) ||
        !add(mapped, workers + service, 2U * page) ||
        !add(ordinary, workers + service, 3U * ((kernel + word_bits - 1U) / word_bits) * sizeof(unsigned long)))
        return std::unexpected{Reason::capacity};
    if (capacity.context_requested() &&
        (!add(mapped, workers, capacity.perf_mapping_bytes()) || !add(descriptors, workers) ||
         !add(ordinary, workers, capacity.perf_record_capacity() * sizeof(platform_linux::PerfContextSwitchRecord))))
        return std::unexpected{Reason::capacity};
    if (capacity.frequency_requested() && !add(descriptors, 1U + 2U * domains))
        return std::unexpected{Reason::capacity};
    if (capacity.temperature_requested() && !add(descriptors, 3U))
        return std::unexpected{Reason::capacity};
    // Payloads for the existing fixed136 worker command generations, their
    // result/failure/signal rows and bounded scalar owners. No allocator metadata
    // or compiler frame equation is asserted by this conservative allowance.
    if (!add(ordinary, workers, 136U * 1024U) || !add(ordinary, 1U, 65536U))
        return std::unexpected{Reason::capacity};
    if (const auto* latency = std::get_if<runtime::ipc::PlacementLatencyExecutionInputs>(
            &placement.execution_inputs().kernel())) {
        // Successors + verification permutation/inverse coexist; the earlier
        // cycle-check transient is smaller. Nodes are64 bytes, not8-byte words.
        const auto logical = latency->configuration().working_set().value();
        if (logical % 64U || !add(ordinary, logical / 64U, 16U + sizeof(std::size_t)))
            return std::unexpected{Reason::capacity};
    }
    // Actual public payload types for the existing monitoring stock. Dynamic
    // token/membership payloads are charged separately below.
    for (std::size_t index{}; index < static_cast<std::size_t>(Slot::count); ++index) {
        const auto slot = static_cast<Slot>(index);
        std::uint64_t width{};
        switch (slot) {
        case Slot::raw_page_before: case Slot::raw_page_after:
            width = sizeof(mon::MonitoringRawCaptureEvidence::PageAcquisition) + sizeof(std::string) + sizeof("linux-task-stat-page-faults"); break;
        case Slot::raw_context:
            width = sizeof(mon::MonitoringRawCaptureEvidence::ContextAcquisition) + sizeof("linux-perf-context-switch-records") + 1U; break;
        case Slot::context_events: width = sizeof(mon::ContextSwitchEvent); break;
        case Slot::raw_temperature:
            width = sizeof(mon::MonitoringRawCaptureEvidence::TemperatureAcquisition) + sizeof(std::string) + sizeof("linux-hwmon-k10temp-tctl") + 1U; break;
        case Slot::raw_frequency_domains:
            width = sizeof(std::vector<mon::FrequencyAcquisition>) + sizeof(mon::FrequencyDomainAcquisitions) + sizeof(mon::FrequencyPolicyMetadata) + 2U * sizeof(std::size_t) + 2U * sizeof(std::string); break;
        case Slot::raw_frequency_acquisitions:
            width = sizeof(mon::FrequencyAcquisition) + sizeof(std::string) + page + 1U; break;
        case Slot::classified_page_workers: width = sizeof(mon::WorkerPageFaultEvidence); break;
        case Slot::classified_context_workers:
            width = sizeof(mon::WorkerContextSwitchEvidence) + sizeof(std::string) + sizeof("linux-perf-context-switch-records"); break;
        case Slot::classified_frequency_domains: width = sizeof(mon::FrequencyDomainEvidence); break;
        case Slot::page_pair_scratch: width = sizeof(mon::PageFaultAcquisitionPair); break;
        case Slot::context_worker_scratch: width = sizeof(core::WorkerId); break;
        case Slot::context_view_scratch: width = sizeof(mon::ContextSwitchWorkerAcquisitionView); break;
        case Slot::context_offsets: case Slot::context_lookup_scratch: width = sizeof(std::size_t); break;
        case Slot::temperature_view_scratch: case Slot::frequency_view_scratch: width = sizeof(void*); break;
        case Slot::frequency_result_scratch:
            width = sizeof(std::optional<mon::detail::FrequencyDomainClassification>) + sizeof(mon::FrequencyDomainAcquisitions); break;
        case Slot::persistent_worker_scopes: width = sizeof(mon::MonitoringWorkerScope); break;
        case Slot::persistent_page_sources: width = sizeof(rt::PairwiseTaskStatWorkspace); break;
        case Slot::persistent_context_sources: width = sizeof(rt::PairwisePreparedPerfContextSwitch) + sizeof(rt::PairwisePerfDrainStorage) + 1024U; break;
        case Slot::persistent_context_state: width = sizeof(platform_linux::RawMonotonicTimestamp); break;
        case Slot::persistent_temperature_sources: case Slot::persistent_frequency_sources:
            width = sizeof(rt::PairwiseSysfsReadWorkspace) + page + 1U; break;
        case Slot::persistent_frequency_metadata: width = sizeof(mon::FrequencyPolicyMetadata) + 2U * page; break;
        case Slot::frequency_nested_cpu_ids: case Slot::persistent_frequency_cpu_ids: case Slot::context_membership_scratch:
            width = sizeof(core::LinuxCpuId); break;
        case Slot::retained_frequency_raw_bytes: case Slot::retained_frequency_escaped_bytes: width = 1U; break;
        case Slot::count: return std::unexpected{Reason::capacity};
        }
        if (!add(ordinary, capacity.slot(slot), width)) return std::unexpected{Reason::capacity};
    }
    // Existing bounded discovery's project-controlled rows/strings and actual
    // operation/retirement stocks are preparation peaks, including empty hosts.
    if (capacity.frequency_requested()) {
        if (!add(ordinary, kernel, 12U * page + 1024U) ||
            !add(ordinary, members, 4U * sizeof(core::LinuxCpuId)) ||
            !add(ordinary, workers, sizeof(core::LinuxCpuId)) || !add(ordinary, 2U, page + 1U) ||
            !add(ordinary, entries + 3U + 41U * kernel + 2U,
                 sizeof(platform_linux::detail::BoundedSysfsDisposition)))
            return std::unexpected{Reason::capacity};
    }
    if (capacity.temperature_requested()) {
        const auto shape = platform_linux::detail::bounded_hwmon_storage_shape({present, entries, page});
        if (!shape || !add(ordinary, entries, 2U * (page + 1U)) || !add(ordinary, 8U, page) ||
            !add(ordinary, shape->operation_capacity,
                 sizeof(platform_linux::detail::BoundedSysfsDisposition)) ||
            !add(ordinary, shape->retirement_capacity,
                 sizeof(platform_linux::detail::BoundedSysfsDisposition)))
            return std::unexpected{Reason::capacity};
    }
    std::uint64_t failure_rows = workers + 16U;
    if (!add(failure_rows, capacity.preparation_rollback_values()) ||
        !add(failure_rows, capacity.completion_abort_values()) ||
        !add(failure_rows, capacity.whole_stop_values()) || !add(failure_rows, domains, 4U))
        return std::unexpected{Reason::capacity};
    if (!add(ordinary, capacity.capture_slots(),
             sizeof(mon::detail::PairwisePreparedCaptureSlot) + 1024U) ||
        !add(ordinary, capacity.capture_slots() * (capacity.round_capacity() + 3U),
             sizeof(std::shared_ptr<std::vector<core::LinuxCpuId>>) + sizeof(std::vector<core::LinuxCpuId>)) ||
        !add(ordinary, failure_rows, 1024U))
        return std::unexpected{Reason::capacity};
    std::uint64_t total = stack;
    if (!add(total, mapped) || !add(total, ordinary) || descriptors > SIZE_MAX)
        return std::unexpected{Reason::capacity};
    return work::Resources{0U, static_cast<std::size_t>(workers + service),
        static_cast<std::size_t>(workers), static_cast<std::size_t>(service), 0U,
        static_cast<std::size_t>(descriptors), Byte::from_base_units(stack), Byte::from_base_units(mapped),
        Byte::from_base_units(ordinary), Byte::from_base_units(total)};
}

struct HostFrozenCollectionState final {
    HostFrozenCollectionState(HostFrozenPhase phase_value, std::optional<HostFrozenSourceInputs> input,
        work::ResourceLedger& ledger_value, runtime::RunnerCancellation& cancel,
        HostFrozenCollectionDependencies dependencies_value,
        std::uint64_t begin, std::uint64_t end)
        : phase{phase_value}, source{input}, ledger{ledger_value}, cancellation{cancel},
          dependencies{std::move(dependencies_value)}, latest{begin}, phase_deadline{end} {
        trials.reserve(5U);
    }
    HostFrozenPhase phase;
    std::optional<HostFrozenSourceInputs> source;
    std::optional<HostFrozenSourceInputs> owned_synthetic;
    std::span<const HostFrozenCollection::Endpoint> synthetic;
    work::ResourceLedger& ledger;
    runtime::RunnerCancellation& cancellation;
    HostFrozenCollectionDependencies dependencies;
    std::optional<HostFrozenObservationAdmission> pending;
    std::vector<measurement::EmptyHarnessTrial> trials;
    std::optional<measurement::TimerOverheadEstimate> estimate;
    std::array<std::optional<statistics::Bandwidth>, 4> bandwidth;
    std::array<std::optional<statistics::DependentLoadLatency>, 4> latency;
    std::size_t consumed{}, representatives{}, pairs{};
    bool outstanding{}, completed{};
    std::optional<Reason> failure;
    std::uint64_t latest{}, phase_deadline{}, comparison_deadline{}, block_deadline{};
    auto domain() const noexcept -> HostFrozenDomain {
        return source ? HostFrozenDomain::source : HostFrozenDomain::synthetic;
    }
    auto control_inputs() const noexcept -> const HostFrozenSourceInputs* {
        if (source) return &*source;
        return owned_synthetic ? &*owned_synthetic : nullptr;
    }
    void fail(Reason reason) noexcept { if (!failure) failure = reason; }
    auto guard() noexcept -> std::optional<Reason> {
        if (auto failed = sample_guard(dependencies.guard, cancellation, latest)) return failed;
        if ((phase_deadline && latest >= phase_deadline) ||
            (control_inputs() && control_inputs()->control->permit_deadline() &&
             latest >= control_inputs()->control->permit_deadline()) ||
            (comparison_deadline && latest >= comparison_deadline) ||
            (block_deadline && latest >= block_deadline)) {
            (void)cancellation.try_request(runtime::ipc::CancelReason::controller_timeout);
            return Reason::deadline;
        }
        return std::nullopt;
    }
    auto deadline(std::uint64_t seconds) const noexcept -> std::optional<std::uint64_t> {
        return absolute_deadline(latest, seconds);
    }
};
struct HostFrozenCellState final {
    HostFrozenCellState(std::shared_ptr<HostFrozenCollectionState> owner_value,
                       HostFrozenCellIdentity identity_value)
        : owner{std::move(owner_value)}, id{identity_value} {}
    std::shared_ptr<HostFrozenCollectionState> owner;
    HostFrozenCellIdentity id;
    std::optional<HostFrozenObservationAdmission> admission;
    const runtime::AdmittedPlacementRequest* placement{};
    std::optional<rt::FrozenObservation> observation;
    std::optional<rt::FrozenObservationTerminalFacts> terminal;
    std::optional<HostFrozenBlockRepresentatives> representatives;
    std::optional<work::ResourceLease> storage_lease;
    bool consumed{};
    ~HostFrozenCellState() noexcept {
        // Native retirement is confirmed before a Cell can be returned. Its
        // evidence payloads must end before ordinary reservation reuse.
        observation.reset(); terminal.reset(); admission.reset(); representatives.reset();
        if (storage_lease && !storage_lease->retire()) owner->fail(Reason::retirement);
    }
};
struct HostFrozenFailureState final {
    explicit HostFrozenFailureState(std::shared_ptr<HostFrozenCollectionState> owner_value)
        : owner{std::move(owner_value)} {}
    std::shared_ptr<HostFrozenCollectionState> owner;
    Reason reason{Reason::unavailable};
    std::optional<HostFrozenObservationAdmission> admission;
    std::optional<rt::FrozenObservationTerminalFacts> terminal;
    std::optional<rt::FrozenObservationRetirement> retirement;
    std::optional<work::ResourceLease> lease;
    std::optional<work::ResourceLease> storage_lease;
    bool storage_retirable{true};
    std::optional<platform_linux::detail::LinuxMonitoringCapacityFailure> capacity;
    std::optional<work::Failure> headroom;
    std::optional<HostFrozenAdmissionReason> admission_reason;
    HostFrozenBindingCheckpoint binding_checkpoint{HostFrozenBindingCheckpoint::none};
    std::optional<core::Error> error;
    std::optional<rt::FrozenObservation> observation;
    std::optional<rt::PairwiseMonitoringHostCapacityAdmissionFailure> binding;
    std::optional<runtime::PairwiseRuntimeFailure> projection;
    void retain_admission_failure(HostFrozenAdmissionFailure&& failure) noexcept {
        admission_reason = failure.reason;
        binding_checkpoint = failure.binding_checkpoint;
        error = std::move(failure.lower);
    }
    // Lower owner is destroyed before admission/lease, including uncertain RAII
    // teardown. That teardown never turns a retained charge into retirement.
    ~HostFrozenFailureState() noexcept {
        retirement.reset();
        if (error)
            core::detail::retire_json(error->context);
        observation.reset(); terminal.reset(); admission.reset();
        capacity.reset(); headroom.reset(); error.reset(); binding.reset(); projection.reset();
        if (storage_retirable && storage_lease && !storage_lease->retire())
            owner->fail(Reason::retirement);
        // Otherwise ordinary ownership was not confirmed safe to retire. The
        // existing lease abandonment poisons the graph; no reuse is authorized.
    }
};

auto production_host_frozen_capacity_operations() noexcept -> HostFrozenCapacityOperations {
    return {nullptr, [](void*, const runtime::AdmittedPlacementRequest& input,
                      runtime::PairwiseMonitoringCapacityAcquisitionLeg) noexcept {
        return rt::PairwiseMonitoringHostCapacityAdmissionAccess::mint_single(input);
    }};
}
HostFrozenCell::HostFrozenCell(std::unique_ptr<HostFrozenCellState> state) noexcept
    : state_{std::move(state)} {}
HostFrozenCell::HostFrozenCell(HostFrozenCell&&) noexcept = default;
HostFrozenCell::~HostFrozenCell() noexcept {
    if (state_ && !state_->consumed) {
        state_->owner->fail(Reason::abandoned);
        state_->owner->outstanding = false;
    }
}
auto HostFrozenCell::identity() const noexcept -> std::optional<HostFrozenCellIdentity> {
    return state_ ? std::optional{state_->id} : std::nullopt;
}
auto HostFrozenCell::domain() const noexcept -> HostFrozenDomain {
    return state_ ? state_->owner->domain() : HostFrozenDomain::synthetic;
}
auto HostFrozenCell::admission() const noexcept -> const HostFrozenObservationAdmission* {
    return state_ && state_->admission ? &*state_->admission : nullptr;
}
auto HostFrozenCell::placement() const noexcept -> const runtime::AdmittedPlacementRequest* {
    return state_ ? state_->placement : nullptr;
}
auto HostFrozenCell::observation() const noexcept -> const rt::FrozenObservation* {
    return state_ && state_->observation ? &*state_->observation : nullptr;
}
auto HostFrozenCell::terminal_facts() const noexcept -> const rt::FrozenObservationTerminalFacts* {
    return state_ && state_->terminal ? &*state_->terminal : nullptr;
}
auto HostFrozenCell::empty_trials() const noexcept -> std::span<const measurement::EmptyHarnessTrial> {
    return state_ ? std::span<const measurement::EmptyHarnessTrial>{state_->owner->trials}
                  : std::span<const measurement::EmptyHarnessTrial>{};
}
auto HostFrozenCell::timer_estimate() const noexcept -> std::optional<measurement::TimerOverheadEstimate> {
    return state_ ? state_->owner->estimate : std::nullopt;
}
auto HostFrozenCell::representatives() const noexcept -> const HostFrozenBlockRepresentatives* {
    return state_ && state_->representatives ? &*state_->representatives : nullptr;
}
HostFrozenCollectionFailure::HostFrozenCollectionFailure(Reason reason) noexcept : reason_{reason} {}
HostFrozenCollectionFailure::HostFrozenCollectionFailure(std::unique_ptr<HostFrozenFailureState> state) noexcept
    : reason_{state->reason}, state_{std::move(state)} {}
HostFrozenCollectionFailure::HostFrozenCollectionFailure(HostFrozenCollectionFailure&&) noexcept = default;
HostFrozenCollectionFailure::~HostFrozenCollectionFailure() noexcept = default;
auto HostFrozenCollectionFailure::reason() const noexcept -> Reason { return reason_; }
auto HostFrozenCollectionFailure::terminal_facts() const noexcept -> const rt::FrozenObservationTerminalFacts* {
    return state_ && state_->terminal ? &*state_->terminal : nullptr;
}
auto HostFrozenCollectionFailure::retained_retirement() const noexcept -> const rt::FrozenObservationRetirement* {
    return state_ && state_->retirement ? &*state_->retirement : nullptr;
}
auto HostFrozenCollectionFailure::capacity_failure() const noexcept
    -> const platform_linux::detail::LinuxMonitoringCapacityFailure* {
    return state_ && state_->capacity ? &*state_->capacity : nullptr;
}
auto HostFrozenCollectionFailure::headroom_failure() const noexcept -> const work::Failure* {
    return state_ && state_->headroom ? &*state_->headroom : nullptr;
}
auto HostFrozenCollectionFailure::admission_reason() const noexcept
    -> std::optional<HostFrozenAdmissionReason> {
    return state_ ? state_->admission_reason : std::nullopt;
}
auto HostFrozenCollectionFailure::binding_checkpoint() const noexcept -> HostFrozenBindingCheckpoint {
    return state_ ? state_->binding_checkpoint : HostFrozenBindingCheckpoint::none;
}
auto HostFrozenCollectionFailure::lower_error() const noexcept -> const core::Error* {
    return state_ && state_->error ? &*state_->error : nullptr;
}
auto HostFrozenCollectionFailure::charge_retained() const noexcept -> bool {
    return state_ && state_->lease && state_->lease->active();
}
auto HostFrozenCollectionFailure::observation() const noexcept -> const rt::FrozenObservation* {
    return state_ && state_->observation ? &*state_->observation : nullptr;
}
auto HostFrozenCollectionFailure::admission() const noexcept -> const HostFrozenObservationAdmission* {
    return state_ && state_->admission ? &*state_->admission : nullptr;
}
auto HostFrozenCollectionFailure::binding_failure() const noexcept
    -> std::optional<rt::PairwiseMonitoringHostCapacityAdmissionFailure> {
    return state_ ? state_->binding : std::nullopt;
}
auto HostFrozenCollectionFailure::projection_failure() const noexcept -> const runtime::PairwiseRuntimeFailure* {
    return state_ && state_->projection ? &*state_->projection : nullptr;
}
HostFrozenCollection::HostFrozenCollection(std::shared_ptr<HostFrozenCollectionState> state) noexcept
    : state_{std::move(state)} {}
HostFrozenCollection::HostFrozenCollection(HostFrozenCollection&&) noexcept = default;
HostFrozenCollection::~HostFrozenCollection() noexcept = default;

auto HostFrozenCollection::retain_admission_failure_for_testing(HostFrozenAdmissionFailure&& failure)
    -> HostFrozenCollectionFailure {
    auto state = std::make_unique<HostFrozenFailureState>(nullptr);
    state->reason = Reason::admission;
    state->retain_admission_failure(std::move(failure));
    return HostFrozenCollectionFailure{std::move(state)};
}

auto HostFrozenCollection::prepare_q5_source(HostFrozenSourceInputs input, work::ResourceLedger& ledger,
    runtime::RunnerCancellation& cancel, HostFrozenCollectionDependencies dependencies) noexcept
    -> std::expected<HostFrozenCollection, HostFrozenCollectionFailure> {
    return prepare(HostFrozenPhase::q5, input, {}, ledger, cancel, std::move(dependencies));
}
auto HostFrozenCollection::prepare_q6_source(HostFrozenSourceInputs input, work::ResourceLedger& ledger,
    runtime::RunnerCancellation& cancel, HostFrozenCollectionDependencies dependencies) noexcept
    -> std::expected<HostFrozenCollection, HostFrozenCollectionFailure> {
    return prepare(HostFrozenPhase::q6, input, {}, ledger, cancel, std::move(dependencies));
}
auto HostFrozenCollection::prepare_owned_synthetic(HostFrozenPhase phase,
    std::span<const Endpoint> endpoints, HostFrozenSourceInputs input,
    work::ResourceLedger& ledger, runtime::RunnerCancellation& cancel,
    HostFrozenCollectionDependencies dependencies) noexcept
    -> std::expected<HostFrozenCollection, HostFrozenCollectionFailure> {
    return prepare(phase, std::nullopt, endpoints, ledger, cancel,
                   std::move(dependencies), input);
}
auto HostFrozenCollection::prepare(HostFrozenPhase phase, std::optional<HostFrozenSourceInputs> source,
    std::span<const Endpoint> synthetic, work::ResourceLedger& ledger,
    runtime::RunnerCancellation& cancel, HostFrozenCollectionDependencies dependencies,
    std::optional<HostFrozenSourceInputs> owned_synthetic) noexcept
    -> std::expected<HostFrozenCollection, HostFrozenCollectionFailure> {
    if ((phase != HostFrozenPhase::q5 && phase != HostFrozenPhase::q6) ||
        !valid_dependencies(dependencies) || (!source && synthetic.size() != 10U) ||
        (source && owned_synthetic) || ledger.poisoned())
        return std::unexpected{HostFrozenCollectionFailure{Reason::invalid_input}};
    // The source endpoint derives both its demand and lower preparation from
    // the same genuine capacity, regardless of caller-supplied discovery sizes.
    if (source || owned_synthetic) dependencies.run.source_capacity_from_admission = true;
    std::optional<std::uint64_t> original_end;
    if (source) {
        if (!source->control || !source->environment)
            return std::unexpected{HostFrozenCollectionFailure{Reason::association}};
        auto checked = source->control->phase_deadline(*source, phase, cancel);
        if (!checked) return std::unexpected{HostFrozenCollectionFailure{checked.error()}};
        original_end = *checked;
    } else if (owned_synthetic) {
        if (!owned_synthetic->control)
            return std::unexpected{HostFrozenCollectionFailure{Reason::association}};
        auto checked = owned_synthetic->control->phase_deadline_owned(*owned_synthetic, phase, cancel);
        if (!checked) return std::unexpected{HostFrozenCollectionFailure{checked.error()}};
        original_end = *checked;
    }
    // The fixed phase window includes State storage and genuine source admission.
    // These stack facts exist before either operation can allocate or consume time.
    std::uint64_t begin{};
    if (auto failed = sample_guard(dependencies.guard, cancel, begin))
        return std::unexpected{HostFrozenCollectionFailure{*failed}};
    const auto end = source || owned_synthetic ? original_end :
        absolute_deadline(begin, phase == HostFrozenPhase::q5 ? 600U : 6000U);
    if (!end) return std::unexpected{HostFrozenCollectionFailure{Reason::clock}};
    try {
        auto state = std::make_shared<HostFrozenCollectionState>(phase, source, ledger, cancel,
                                                                std::move(dependencies), begin, *end);
        state->synthetic = synthetic;
        if (owned_synthetic) state->owned_synthetic.emplace(*owned_synthetic);
        if (source) {
            const auto id = identity(phase, 0U);
            auto made = HostFrozenObservationAdmission::create_source(
                source->session, *source->environment, source->plan, source->calibration,
                source->public_values, source->request_ids, id.family, id.configuration,
                source->raw_failure);
            if (!made) {
                auto failure = std::make_unique<HostFrozenFailureState>(state);
                failure->reason = Reason::admission;
                failure->retain_admission_failure(std::move(made.error()));
                return std::unexpected{HostFrozenCollectionFailure{std::move(failure)}};
            }
            state->pending.emplace(std::move(*made));
        } else for (const auto& endpoint : synthetic) {
            if (!endpoint.placement || !endpoint.policy || !endpoint.service)
                return std::unexpected{HostFrozenCollectionFailure{Reason::invalid_input}};
        }
        if (auto failed = state->guard())
            return std::unexpected{HostFrozenCollectionFailure{*failed}};
        return HostFrozenCollection{std::move(state)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{HostFrozenCollectionFailure{Reason::allocation}};
    } catch (...) {
        return std::unexpected{HostFrozenCollectionFailure{Reason::contained_exception}};
    }
}

auto HostFrozenCollection::next() noexcept
    -> std::expected<std::optional<HostFrozenCell>, HostFrozenCollectionFailure> {
    if (!state_) return std::unexpected{HostFrozenCollectionFailure{Reason::unavailable}};
    auto& state = *state_;
    if (state.failure) return std::unexpected{HostFrozenCollectionFailure{*state.failure}};
    if (state.outstanding) {
        state.fail(Reason::outstanding);
        return std::unexpected{HostFrozenCollectionFailure{Reason::outstanding}};
    }
    if (state.completed) return std::optional<HostFrozenCell>{};
    const auto id = identity(state.phase, state.consumed);
    std::unique_ptr<HostFrozenFailureState> failure;
    std::unique_ptr<HostFrozenCellState> cell;
    std::optional<rt::FrozenObservationRun> run;
    bool native_started{};
    const auto fail = [&](Reason reason) -> std::expected<std::optional<HostFrozenCell>, HostFrozenCollectionFailure> {
        state.fail(reason);
        if (!failure) return std::unexpected{HostFrozenCollectionFailure{reason}};
        if (!native_started && failure->lease) {
            // A prepared Run owns ordinary buffers only. Destruction precedes
            // count/native reservation release on every pre-activation refusal.
            run.reset();
            if (!failure->lease->retire()) state.fail(Reason::retirement);
            failure->lease.reset();
        }
        failure->reason = *state.failure;
        if (cell && cell->admission) failure->admission.emplace(std::move(*cell->admission));
        if (cell && cell->observation) failure->observation.emplace(std::move(*cell->observation));
        if (cell && cell->terminal && !failure->terminal)
            failure->terminal.emplace(std::move(*cell->terminal));
        return std::unexpected{HostFrozenCollectionFailure{std::move(failure)}};
    };
    // Preserve existing later-slot origins; capture only a new cell/block or
    // comparison before allocating its owning failure and output wrappers.
    if (auto failed = state.guard()) return fail(*failed);
    if (id.slot == 0U) {
        if (id.block == 0U) {
            auto deadline = state.deadline(720U);
            if (!deadline) return fail(Reason::clock);
            state.comparison_deadline = state.phase == HostFrozenPhase::q6 ? *deadline : 0U;
        }
        auto deadline = state.deadline(60U);
        if (!deadline) return fail(Reason::clock);
        state.block_deadline = *deadline;
    }
    try {
        failure = std::make_unique<HostFrozenFailureState>(state_);
        cell = std::make_unique<HostFrozenCellState>(state_, id);
        if (auto failed = state.guard()) return fail(*failed);
        if (!state.estimate || (state.phase == HostFrozenPhase::q6 && id.block == 0U && id.slot == 0U)) {
            state.trials.clear(); state.estimate.reset();
            std::uint64_t minimum = UINT64_MAX;
            for (std::size_t i{}; i < 5U; ++i) {
                if (auto failed = state.guard()) return fail(*failed);
                auto trial = measurement::detail::EmptyHarnessAccess::run(state.dependencies.empty_clock);
                if (!trial) { failure->error.emplace(std::move(trial.error())); return fail(Reason::clock); }
                minimum = std::min(minimum, trial->interval().elapsed().value());
                state.trials.push_back(std::move(*trial));
                if (auto failed = state.guard()) return fail(*failed);
            }
            state.estimate.emplace(measurement::TimerOverheadEstimate::create(core::Duration::from_base_units(minimum)));
        }
        const MonitoringExecutionPolicy* policy{};
        const runtime::ServicePlacementPlan* services{};
        std::optional<HostFrozenWork> selected;
        if (state.source) {
            if (state.pending) { cell->admission.emplace(std::move(*state.pending)); state.pending.reset(); }
            else {
                const auto& input = *state.source;
                auto made = HostFrozenObservationAdmission::create_source(
                    input.session, *input.environment, input.plan, input.calibration,
                    input.public_values, input.request_ids, id.family, id.configuration,
                    input.raw_failure);
                if (!made) {
                    failure->retain_admission_failure(std::move(made.error()));
                    return fail(Reason::admission);
                }
                cell->admission.emplace(std::move(*made));
            }
            auto view = cell->admission->view();
            if (!view) return fail(Reason::admission);
            cell->placement = &view->placement.get(); policy = &view->policy.get(); services = &view->service.get();
            selected.emplace(view->work.get());
        } else {
            const auto index = (id.family == HostQ0bFamily::bandwidth ? 0U : 5U) +
                               static_cast<std::size_t>(id.configuration);
            const auto& endpoint = state.synthetic[index];
            cell->placement = endpoint.placement; policy = endpoint.policy; services = endpoint.service;
            selected.emplace(endpoint.work);
        }
        if (auto failed = state.guard()) return fail(*failed);
        std::vector<core::LinuxNumaNodeId> nodes;
        nodes.reserve(cell->placement->workers().size());
        for (const auto& worker : cell->placement->workers())
            for (const auto& node : worker.request().region().nodes())
                if (std::ranges::find(nodes, node.linux_numa_node()) == nodes.end()) nodes.push_back(node.linux_numa_node());
        auto headroom = work::read_headroom(nodes, state.dependencies.headroom);
        if (!headroom) { failure->headroom.emplace(headroom.error()); return fail(Reason::headroom); }
        if (auto failed = state.guard()) return fail(*failed);
        auto controller = state.dependencies.capacity.mint(state.dependencies.capacity.context,
            *cell->placement, runtime::PairwiseMonitoringCapacityAcquisitionLeg::first_controller);
        if (!controller) { failure->capacity.emplace(controller.error()); return fail(Reason::capacity); }
        if (auto failed = state.guard()) return fail(*failed);
        auto runner = state.dependencies.capacity.mint(state.dependencies.capacity.context,
            *cell->placement, runtime::PairwiseMonitoringCapacityAcquisitionLeg::second_runner);
        if (!runner) { failure->capacity.emplace(runner.error()); return fail(Reason::capacity); }
        auto admitted = rt::PairwiseMonitoringHostCapacityAdmissionAccess::project_single(
            *cell->placement, *policy, *services, std::move(*controller), std::move(*runner),
            rt::PairwiseMonitoringHostCapacityAdmissionAccess::expected_capacity_identity(),
            rt::PairwiseMonitoringHostCapacityAdmissionAccess::expected_profile_identity());
        if (!admitted) { failure->binding = admitted.error(); return fail(Reason::capacity); }
        auto capacity = rt::PairwiseMonitoringCapacityAccess::project(std::move(*admitted));
        if (!capacity) { failure->projection.emplace(capacity.error()); return fail(Reason::capacity); }
        if (state.source) {
            auto gated = state.source->control->before_prepare(*state.source, id, state.cancellation);
            if (!gated) return fail(gated.error());
        } else if (state.owned_synthetic) {
            auto gated = state.owned_synthetic->control->before_prepare_owned(
                *state.owned_synthetic, id, state.cancellation);
            if (!gated) return fail(gated.error());
        }
        std::size_t service_count{};
        for (const auto& role : services->assignments())
            if (role.role == runtime::ServiceRole::measurement_monitor) ++service_count;
        const auto workers = cell->placement->workers().size();
        work::Resources demand{0U, workers + service_count, workers, service_count, 0U};
        const bool source_stock = state.source || state.dependencies.run.source_capacity_from_admission;
        if (source_stock) {
            auto checked = host_frozen_run_resources(*cell->placement, *capacity);
            if (!checked) return fail(checked.error());
            demand = *checked;
            work::Resources storage{};
            storage.ordinary_bytes = demand.ordinary_bytes;
            storage.address_space_bytes = storage.ordinary_bytes;
            auto lease = state.ledger.reserve(storage);
            if (!lease) return fail(Reason::capacity);
            failure->storage_lease.emplace(std::move(*lease));
            demand.ordinary_bytes = core::ByteCount::from_base_units(0U);
            const auto native_bytes = demand.stack_bytes.checked_add(demand.mapped_bytes);
            if (!native_bytes) return fail(Reason::capacity);
            demand.address_space_bytes = *native_bytes;
        }
        auto lease = state.ledger.reserve(demand);
        if (!lease) return fail(Reason::capacity);
        failure->lease.emplace(std::move(*lease));
        auto prepared = rt::FrozenObservationRun::prepare(*cell->placement, *policy, *services,
            std::move(*capacity), *selected, 1U, *state.estimate, state.cancellation, state.dependencies.run);
        if (!prepared) return fail(Reason::admission);
        run.emplace(std::move(*prepared));
        if (auto failed = state.guard()) return fail(*failed);
        if (const auto* input = state.control_inputs()) {
            auto gated = state.source
                ? input->control->before_activate(*input, id, state.cancellation)
                : input->control->before_activate_owned(*input, id, state.cancellation);
            if (!gated) {
                // No activation occurred; discard prepared Run storage before
                // returning the reservation. Later native retirement stays below.
                run.reset();
                if (!failure->lease->retire()) return fail(Reason::retirement);
                failure->lease.reset();
                return fail(gated.error());
            }
        }
        native_started = true;
        failure->storage_retirable = false;
        auto activated = run->activate();
        if (!activated) state.fail(activated.error() == rt::FrozenObservationRunReason::cancelled
            ? Reason::cancelled : Reason::activation);
        if (auto failed = state.guard()) state.fail(*failed);
        if (!state.failure) {
            const auto observation_id = core::ObservationId{state.consumed + 1U};
            const auto experiment = core::ExperimentId{id.comparison + 1U};
            const auto block = plan(id.comparison, id.block).block_id();
            auto observed = id.family == HostQ0bFamily::bandwidth
                ? run->observe_bandwidth(0U, observation_id, experiment, block)
                : run->observe_latency(0U, observation_id, experiment, block);
            if (!observed) state.fail(observed.error() == rt::FrozenObservationRunReason::cancelled
                ? Reason::cancelled : Reason::observation);
            else cell->observation.emplace(std::move(*observed));
        }
        if (auto failed = state.guard()) state.fail(*failed);
        auto retired = std::move(*run).finish(); run.reset();
        if (!retired) return fail(Reason::retirement);
        failure->retirement.emplace(std::move(*retired));
        if (auto failed = state.guard()) state.fail(*failed);
        auto detached = std::move(*failure->retirement).detach_confirmed(
            cell->observation ? &cell->observation->monitoring : nullptr);
        if (!detached) return fail(Reason::retirement);
        failure->retirement.reset();
        failure->storage_retirable = true;
        failure->terminal.emplace(std::move(*detached));
        auto released = failure->lease->retire();
        if (!released) return fail(Reason::retirement);
        failure->lease.reset();
        if (!failure->terminal->failures.empty()) state.fail(Reason::retirement);
        if (failure->terminal->metadata == rt::FrozenObservationMetadataComparison::mismatch)
            state.fail(Reason::metadata);
        if (auto failed = state.guard()) state.fail(*failed);
        if (state.failure) return fail(*state.failure);
        cell->terminal.emplace(std::move(*failure->terminal)); failure->terminal.reset();
        if (cell->observation->validity.disposition() == measurement::ObservationValidityDisposition::invalid)
            state.fail(Reason::invalid_observation);
        if (state.phase == HostFrozenPhase::q6 && !state.failure) {
            auto metric = std::visit([&](const auto& observation) -> std::expected<void, core::Error> {
                using Observation = std::remove_cvref_t<decltype(observation)>;
                if constexpr (std::same_as<Observation, measurement::SequentialReadBandwidthObservation>) {
                    auto value = statistics::Bandwidth::create(observation.aggregate_work(), observation.enclosing_interval().elapsed());
                    if (!value) return std::unexpected{std::move(value.error())};
                    state.bandwidth[id.slot] = *value;
                } else {
                    auto value = statistics::DependentLoadLatency::create(observation.enclosing_interval().elapsed(), observation.aggregate_work());
                    if (!value) return std::unexpected{std::move(value.error())};
                    state.latency[id.slot] = *value;
                }
                return {};
            }, cell->observation->observation);
            if (!metric) { failure->error.emplace(std::move(metric.error())); return fail(Reason::statistics); }
            if (id.slot == 3U) {
                const auto scheduled = plan(id.comparison, id.block);
                std::array<std::size_t, 2> first{}, other{}; std::size_t a{}, b{};
                for (std::size_t i{}; i < 4U; ++i)
                    if (scheduled.observations()[i].comparison_variant() == measurement::ComparisonVariantSlot::first) first[a++] = i;
                    else other[b++] = i;
                if (id.family == HostQ0bFamily::bandwidth) {
                    auto values = statistics::block_bandwidth_representatives(
                        {*state.bandwidth[first[0]], *state.bandwidth[first[1]]},
                        {*state.bandwidth[other[0]], *state.bandwidth[other[1]]});
                    if (!values) { failure->error.emplace(std::move(values.error())); return fail(Reason::statistics); }
                    cell->representatives.emplace(*values);
                } else {
                    auto values = statistics::block_dependent_load_latency_representatives(
                        {*state.latency[first[0]], *state.latency[first[1]]},
                        {*state.latency[other[0]], *state.latency[other[1]]});
                    if (!values) { failure->error.emplace(std::move(values.error())); return fail(Reason::statistics); }
                    cell->representatives.emplace(*values);
                }
            }
        }
        if (failure->storage_lease) {
            cell->storage_lease.emplace(std::move(*failure->storage_lease));
            failure->storage_lease.reset();
        }
        state.outstanding = true;
        return std::optional<HostFrozenCell>{HostFrozenCell{std::move(cell)}};
    } catch (const std::bad_alloc&) { return fail(Reason::allocation); }
      catch (...) { return fail(Reason::contained_exception); }
}

auto HostFrozenCollection::consume_cell(HostFrozenCell&& cell) noexcept
    -> std::expected<void, Reason> {
    if (!state_) return std::unexpected{Reason::unavailable};
    auto& state = *state_;
    if (!cell.state_ || cell.state_->owner.get() != state_.get() || cell.state_->consumed ||
        !state.outstanding || cell.state_->id.ordinal != state.consumed) {
        state.fail(Reason::association); return std::unexpected{Reason::association};
    }
    if (cell.state_->representatives) { state.representatives += 2U; ++state.pairs; }
    cell.state_->consumed = true;
    cell.state_.reset();
    state.outstanding = false;
    ++state.consumed;
    if (auto failed = state.guard()) state.fail(*failed);
    if (state.failure) return std::unexpected{*state.failure};
    state.completed = state.consumed == (state.phase == HostFrozenPhase::q5 ? 8U : 384U);
    if (state.phase == HostFrozenPhase::q5 || state.consumed % 4U == 0U) state.block_deadline = 0U;
    if (state.phase == HostFrozenPhase::q6 && state.consumed % 48U == 0U) state.comparison_deadline = 0U;
    return {};
}
void HostFrozenCollection::fail_output() noexcept { if (state_) state_->fail(Reason::output); }
auto HostFrozenCollection::owns_failure(const HostFrozenCollectionFailure& failure) const noexcept -> bool {
    return state_ && failure.state_ && failure.state_->owner.get() == state_.get();
}
auto HostFrozenCollection::snapshot() const noexcept -> std::optional<HostFrozenCollectionSnapshot> {
    if (!state_) return std::nullopt;
    return HostFrozenCollectionSnapshot{state_->phase, state_->domain(), state_->consumed,
        state_->phase == HostFrozenPhase::q5 ? 8U : 384U, state_->representatives, state_->pairs,
        state_->outstanding, state_->failure.has_value() || state_->completed, state_->completed, state_->failure};
}
} // namespace epycbench::monitoring::qualification::detail
