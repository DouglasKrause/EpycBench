#pragma once

#include "epycbench/monitoring/acquisition_driver.hpp"
#include "monitoring/frequency_classifier_kernel.hpp"
#include "monitoring/temperature_classifier_kernel.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::monitoring::detail {

struct PairwisePreparedCaptureDimensions final {
    std::size_t captures{};
    std::size_t workers{};
    std::size_t rounds{};
    std::size_t events_per_drain{};
    std::size_t base_page_bytes{};
    bool page{};
    bool context{};
    bool temperature{};
    bool frequency{};
};

struct PairwisePreparedFrequencyLayoutObservation final {
    std::size_t domain_capacity{};
    std::size_t membership_capacity{};
    std::size_t domain_count{};
    std::size_t offset_count{};
    std::size_t size_count{};
    std::size_t storage_count{};
    std::size_t token_count{};
    std::size_t authored_count{};
    std::size_t implementation_count{};
    std::size_t result_count{};
    bool committed{};
    auto operator<=>(const PairwisePreparedFrequencyLayoutObservation&) const = default;
};

class PairwisePreparedCaptureSlot final {
  public:
    PairwisePreparedCaptureSlot(const PairwisePreparedCaptureSlot&) = delete;
    PairwisePreparedCaptureSlot(PairwisePreparedCaptureSlot&&) noexcept = default;
    auto operator=(const PairwisePreparedCaptureSlot&)
        -> PairwisePreparedCaptureSlot& = delete;
    auto operator=(PairwisePreparedCaptureSlot&&)
        -> PairwisePreparedCaptureSlot& = delete;

    [[nodiscard]] auto context_capacity() const noexcept -> std::size_t {
        return result_.raw_.context_.capacity();
    }
    [[nodiscard]] auto context_offset_capacity() const noexcept -> std::size_t {
        return result_.raw_.context_worker_offsets_.size();
    }
    [[nodiscard]] auto context_event_capacity() const noexcept -> std::size_t {
        return context_event_capacity_;
    }
    [[nodiscard]] auto frequency_nested_cpu_capacity() const noexcept
        -> std::size_t {
        std::size_t result{};
        for (const auto& cpus : frequency_membership_arenas_)
            result += cpus ? cpus->capacity() : 0U;
        return result;
    }
    [[nodiscard]] auto page_evidence_capacity() const noexcept -> std::size_t {
        return page_evidence_storage_.capacity();
    }
    [[nodiscard]] auto context_evidence_capacity() const noexcept
        -> std::size_t { return context_evidence_storage_.capacity(); }
    [[nodiscard]] auto frequency_evidence_capacity() const noexcept
        -> std::size_t { return frequency_evidence_storage_.capacity(); }

  private:
    PairwisePreparedCaptureSlot() = default;
    MonitoringCaptureResult result_;
    std::vector<std::string> page_implementation_ids_;
    std::size_t page_implementation_id_count_{};
    std::vector<std::string> context_view_implementation_ids_;
    std::vector<bool> context_authored_;
    std::vector<WorkerPageFaultEvidence> page_evidence_storage_;
    std::vector<WorkerContextSwitchEvidence> context_evidence_storage_;
    std::size_t context_event_capacity_{};
    std::vector<FrequencyDomainEvidence> frequency_evidence_storage_;
    std::vector<std::shared_ptr<std::vector<core::LinuxCpuId>>>
        frequency_membership_arenas_;
    std::vector<std::size_t> frequency_domain_offsets_;
    std::vector<std::size_t> frequency_domain_sizes_;
    std::vector<std::array<std::string, 2>> frequency_token_storage_;
    std::vector<std::string> temperature_implementation_ids_;
    std::vector<std::string> frequency_implementation_ids_;
    std::vector<bool> temperature_authored_;
    std::vector<bool> frequency_authored_;
    std::vector<bool> committed_rounds_;
    std::size_t frequency_domain_count_{};
    std::size_t frequency_offset_count_{};
    std::size_t frequency_size_count_{};
    std::size_t frequency_storage_count_{};
    std::size_t frequency_token_count_{};
    std::size_t frequency_authored_count_{};
    std::size_t frequency_implementation_count_{};
    std::size_t round_capacity_{};
    bool frequency_partition_committed_{};
    friend class PairwisePreparedCaptureStorageAccess;
};

class PairwisePreparedClassifierScratch final {
  public:
    PairwisePreparedClassifierScratch(const PairwisePreparedClassifierScratch&) = delete;
    PairwisePreparedClassifierScratch(PairwisePreparedClassifierScratch&&) noexcept = default;
    auto operator=(const PairwisePreparedClassifierScratch&)
        -> PairwisePreparedClassifierScratch& = delete;
    auto operator=(PairwisePreparedClassifierScratch&&)
        -> PairwisePreparedClassifierScratch& = delete;

  private:
    PairwisePreparedClassifierScratch() = default;
    std::vector<PageFaultAcquisitionPair> page_pairs_;
    std::vector<core::WorkerId> context_workers_;
    std::vector<ContextSwitchWorkerAcquisitionView> context_views_;
    std::vector<core::LinuxCpuId> context_membership_;
    std::vector<std::size_t> context_lookup_;
    std::vector<const MonitoringRawCaptureEvidence::TemperatureAcquisition*>
        temperature_views_;
    std::vector<const FrequencyAcquisition*> frequency_views_;
    std::vector<FrequencyDomainAcquisitions> frequency_domains_;
    std::vector<std::optional<FrequencyDomainClassification>>
        frequency_results_;
    std::size_t frequency_result_count_{};
    bool frequency_partition_committed_{};
    friend class PairwisePreparedCaptureStorageAccess;
};

class PairwisePreparedCaptureSlots final {
  public:
    PairwisePreparedCaptureSlots(const PairwisePreparedCaptureSlots&) = delete;
    PairwisePreparedCaptureSlots(PairwisePreparedCaptureSlots&&) noexcept = default;
    auto operator=(const PairwisePreparedCaptureSlots&)
        -> PairwisePreparedCaptureSlots& = delete;
    auto operator=(PairwisePreparedCaptureSlots&&)
        -> PairwisePreparedCaptureSlots& = delete;
    [[nodiscard]] auto size() const noexcept -> std::size_t { return slots_.size(); }
    [[nodiscard]] auto available() const noexcept -> std::size_t {
        std::size_t result{};
        for (const auto& slot : slots_)
            result += static_cast<std::size_t>(slot.has_value());
        return result;
    }

  private:
    explicit PairwisePreparedCaptureSlots(
        std::vector<std::optional<PairwisePreparedCaptureSlot>> slots) noexcept
        : slots_{std::move(slots)} {}
    std::vector<std::optional<PairwisePreparedCaptureSlot>> slots_;
    friend class PairwisePreparedCaptureStorageAccess;
};

class PairwisePreparedCaptureStorageAccess final {
  public:
    template <class Payload>
    [[nodiscard]] static auto acquisition(
        MonitoringChannel channel, MonitoringImplementationId implementation,
        MonitoringSamplePhase phase,
        platform_linux::RawMonotonicTimestamp start,
        platform_linux::RawMonotonicTimestamp end,
        MonitoringPayloadResult<Payload> payload) noexcept
        -> TimestampedMonitoringAcquisition<Payload> {
        return TimestampedMonitoringAcquisition<Payload>{
            channel, std::move(implementation), phase, start, end,
            core::Duration::from_base_units(end.nanoseconds() - start.nanoseconds()),
            std::move(payload)};
    }
    [[nodiscard]] static auto page_worker(
        core::WorkerId worker, MonitoringSignalState state,
        std::optional<PageFaultCount> minor,
        std::optional<PageFaultCount> major) noexcept -> WorkerPageFaultEvidence {
        return WorkerPageFaultEvidence{worker, state, minor, major};
    }
    [[nodiscard]] static auto page_evidence(
        MonitoringSignalState state, std::vector<WorkerPageFaultEvidence> workers) noexcept
        -> PageFaultObservationEvidence {
        return PageFaultObservationEvidence{state, std::move(workers)};
    }
    [[nodiscard]] static auto context_worker(
        core::WorkerId worker, MonitoringSignalState state,
        std::optional<ContextSwitchCount> voluntary,
        std::optional<ContextSwitchCount> involuntary) noexcept
        -> WorkerContextSwitchEvidence {
        return WorkerContextSwitchEvidence{worker, state, voluntary, involuntary};
    }
    [[nodiscard]] static auto context_evidence(
        MonitoringSignalState state,
        std::vector<WorkerContextSwitchEvidence> workers) noexcept
        -> ContextSwitchObservationEvidence {
        return ContextSwitchObservationEvidence{state, std::move(workers)};
    }
    [[nodiscard]] static auto temperature_evidence(
        TemperatureSensorKind kind, MonitoringSignalState state,
        std::optional<TemperatureMilliCelsius> minimum,
        std::optional<TemperatureMilliCelsius> maximum,
        std::optional<TemperatureRangeMilliCelsius> range, std::size_t count,
        core::Duration interval, TemperatureCaptureCompleteness completeness) noexcept
        -> TemperatureObservationEvidence {
        (void)kind;
        return TemperatureObservationEvidence{state, minimum, maximum, range, count,
                                              interval, completeness};
    }
    [[nodiscard]] static auto frequency_metadata(
        FrequencyDomainId domain, FrequencySourceKind kind, FrequencyDriverId driver,
        FrequencyGovernorId governor, FrequencyHz minimum, FrequencyHz maximum) noexcept
        -> FrequencyPolicyMetadata {
        return FrequencyPolicyMetadata{std::move(domain), kind, std::move(driver),
                                       std::move(governor), minimum, maximum};
    }
    [[nodiscard]] static auto frequency_evidence(
        MonitoringSignalState state, std::vector<FrequencyDomainEvidence> domains,
        core::Duration interval, FrequencyCaptureCompleteness completeness) noexcept
        -> FrequencyObservationEvidence {
        return FrequencyObservationEvidence{state, std::move(domains), interval,
                                            completeness};
    }
    [[nodiscard]] static auto result(
        const PairwisePreparedCaptureSlot& slot) noexcept
        -> const MonitoringCaptureResult& { return slot.result_; }
    [[nodiscard]] static auto take_result(
        PairwisePreparedCaptureSlot& slot) noexcept -> MonitoringCaptureResult {
        return std::move(slot.result_);
    }
    [[nodiscard]] static auto page_evidence_capacity(
        const MonitoringCaptureResult& result) noexcept -> std::size_t {
        return result.evidence_.page_ ? result.evidence_.page_->workers_.capacity() : 0U;
    }
    [[nodiscard]] static auto context_evidence_capacity(
        const MonitoringCaptureResult& result) noexcept -> std::size_t {
        return result.evidence_.context_ ? result.evidence_.context_->workers_.capacity() : 0U;
    }
    [[nodiscard]] static auto frequency_evidence_capacity(
        const MonitoringCaptureResult& result) noexcept -> std::size_t {
        return result.evidence_.frequency_ ? result.evidence_.frequency_->domains_.capacity() : 0U;
    }
    [[nodiscard]] static auto context_event_capacity(
        const MonitoringRawCaptureEvidence::ContextAcquisition& acquisition) noexcept
        -> std::size_t {
        return acquisition.payload() ? acquisition.payload()->events_.capacity() : 0U;
    }
    [[nodiscard]] static auto frequency_domain_cpu_capacity(
        const FrequencyPolicyMetadata& metadata) noexcept -> std::size_t {
        return metadata.domain_id_.cpus_.capacity();
    }
    [[nodiscard]] static auto page_evidence_data(
        const PairwisePreparedCaptureSlot& slot) noexcept
        -> const WorkerPageFaultEvidence* {
        return slot.page_evidence_storage_.data();
    }
    [[nodiscard]] static auto context_evidence_data(
        const PairwisePreparedCaptureSlot& slot) noexcept
        -> const WorkerContextSwitchEvidence* {
        return slot.context_evidence_storage_.data();
    }
    [[nodiscard]] static auto append_page_evidence(
        PairwisePreparedCaptureSlot& slot, core::WorkerId worker,
        MonitoringSignalState state, std::optional<PageFaultCount> minor,
        std::optional<PageFaultCount> major) noexcept -> bool {
        if (slot.page_evidence_storage_.size() ==
            slot.page_evidence_storage_.capacity())
            return false;
        slot.page_evidence_storage_.push_back(
            WorkerPageFaultEvidence{worker, state, minor, major});
        return true;
    }
    [[nodiscard]] static auto append_page_acquisition(
        PairwisePreparedCaptureSlot& slot, bool before, core::WorkerId worker,
        platform_linux::RawMonotonicTimestamp collection_start,
        platform_linux::RawMonotonicTimestamp collection_end,
        platform_linux::LinuxTaskPageFaultCounters counters) noexcept -> bool {
        if (slot.page_implementation_id_count_ >=
            slot.page_implementation_ids_.size())
            return false;
        auto& destination = before ? slot.result_.raw_.page_before_
                                   : slot.result_.raw_.page_after_;
        if (destination.size() == destination.capacity() ||
            collection_end < collection_start)
            return false;
        auto implementation = MonitoringImplementationId::create(
            std::move(slot.page_implementation_ids_[
                slot.page_implementation_id_count_++]));
        const auto snapshot = PageFaultSnapshot::create(
            worker, PageFaultCount::from_events(counters.minor_faults()),
            PageFaultCount::from_events(counters.major_faults()));
        destination.push_back(TimestampedMonitoringAcquisition<PageFaultSnapshot>{
            MonitoringChannel::page_faults, std::move(implementation),
            before ? MonitoringSamplePhase::before_observation
                   : MonitoringSamplePhase::after_observation,
            collection_start, collection_end,
            core::Duration::from_base_units(
                collection_end.nanoseconds() - collection_start.nanoseconds()),
            MonitoringPayloadResult<PageFaultSnapshot>::available(snapshot)});
        return true;
    }
    [[nodiscard]] static auto append_context_acquisition(
        PairwisePreparedCaptureSlot& slot, std::size_t worker_index,
        std::size_t ordinal, std::size_t rounds, core::WorkerId worker,
        platform_linux::RawMonotonicTimestamp collection_start,
        platform_linux::RawMonotonicTimestamp collection_end,
        std::span<const platform_linux::PerfContextSwitchRecord> records,
        platform_linux::PerfCaptureCompleteness completeness) noexcept -> bool {
        const auto stride = rounds + 1U;
        if (worker_index + 1U >= slot.result_.raw_.context_worker_offsets_.size() ||
            ordinal >= stride || collection_end < collection_start)
            return false;
        const auto storage_index = worker_index * stride + ordinal;
        if (storage_index >= slot.result_.raw_.context_.size())
            return false;
        auto& acquisition = slot.result_.raw_.context_[storage_index];
        if (slot.context_authored_[storage_index])
            return false;
        if (!acquisition.payload_result_.payload_)
            return false;
        auto& batch = *acquisition.payload_result_.payload_;
        auto& events = batch.events_;
        events.clear();
        for (const auto& record : records) {
            if (record.kind() ==
                platform_linux::PerfContextSwitchKind::switch_in)
                continue;
            if (events.size() == events.capacity())
                return false;
            events.push_back(ContextSwitchEvent::create(
                worker, record.timestamp(),
                record.kind() == platform_linux::PerfContextSwitchKind::
                                     involuntary_switch_out
                    ? ContextSwitchKind::involuntary
                    : ContextSwitchKind::voluntary));
        }
        batch.worker_id_ = worker;
        batch.completeness_ =
            completeness == platform_linux::PerfCaptureCompleteness::complete
                ? ContextSwitchCaptureCompleteness::complete
                : ContextSwitchCaptureCompleteness::incomplete;
        acquisition.collection_start_ = collection_start;
        acquisition.collection_end_ = collection_end;
        acquisition.collection_duration_ = core::Duration::from_base_units(
            collection_end.nanoseconds() - collection_start.nanoseconds());
        slot.context_authored_[storage_index] = true;
        return true;
    }
    [[nodiscard]] static auto finalize_context_acquisitions(
        PairwisePreparedCaptureSlot& slot, std::size_t workers,
        std::size_t rounds) noexcept -> bool {
        const auto stride = rounds + 1U;
        if (slot.context_authored_.size() != workers * stride ||
            slot.result_.raw_.context_.size() != slot.context_authored_.size() ||
            slot.result_.raw_.context_worker_offsets_.size() != workers + 1U)
            return false;
        std::size_t destination{};
        for (std::size_t worker{}; worker < workers; ++worker) {
            slot.result_.raw_.context_worker_offsets_[worker] = destination;
            for (std::size_t ordinal{}; ordinal < stride; ++ordinal) {
                const auto source = worker * stride + ordinal;
                if (!slot.context_authored_[source])
                    continue;
                if (destination != source) {
                    std::destroy_at(&slot.result_.raw_.context_[destination]);
                    std::construct_at(
                        &slot.result_.raw_.context_[destination],
                        std::move(slot.result_.raw_.context_[source]));
                }
                ++destination;
            }
        }
        slot.result_.raw_.context_worker_offsets_[workers] = destination;
        slot.result_.raw_.context_count_ = destination;
        return true;
    }
    [[nodiscard]] static auto prepare_page_pairs(
        PairwisePreparedClassifierScratch& scratch,
        std::span<const MonitoringWorkerScope> workers,
        std::span<const MonitoringRawCaptureEvidence::PageAcquisition> before,
        std::span<const MonitoringRawCaptureEvidence::PageAcquisition> after)
        noexcept -> std::optional<std::span<const PageFaultAcquisitionPair>> {
        if (workers.size() != before.size() || workers.size() != after.size() ||
            workers.size() > scratch.page_pairs_.capacity())
            return std::nullopt;
        scratch.page_pairs_.clear();
        for (std::size_t index{}; index < workers.size(); ++index)
            scratch.page_pairs_.emplace_back(workers[index].worker_id(),
                                             before[index], after[index]);
        return std::span<const PageFaultAcquisitionPair>{scratch.page_pairs_};
    }
    [[nodiscard]] static auto prepare_context_views(
        PairwisePreparedClassifierScratch& scratch,
        PairwisePreparedCaptureSlot& slot,
        std::span<const MonitoringWorkerScope> workers,
        std::span<const platform_linux::RawMonotonicTimestamp> coverage_start,
        std::span<const platform_linux::RawMonotonicTimestamp> coverage_end,
        std::span<const ContextSwitchCaptureCompleteness> completeness) noexcept
        -> bool {
        if (workers.size() != coverage_start.size() ||
            workers.size() != coverage_end.size() ||
            workers.size() != completeness.size() ||
            workers.size() > scratch.context_views_.capacity() ||
            workers.size() != slot.context_view_implementation_ids_.size() ||
            slot.result_.raw_.context_worker_offsets_.size() !=
                workers.size() + 1U)
            return false;
        scratch.context_workers_.clear();
        scratch.context_views_.clear();
        for (std::size_t index{}; index < workers.size(); ++index) {
            const auto begin = slot.result_.raw_.context_worker_offsets_[index];
            const auto end = slot.result_.raw_.context_worker_offsets_[index + 1U];
            const auto logical_size = slot.result_.raw_.context_count_.value_or(
                slot.result_.raw_.context_.size());
            if (begin > end || end > logical_size)
                return false;
            scratch.context_workers_.push_back(workers[index].worker_id());
            scratch.context_views_.emplace_back(
                workers[index].worker_id(),
                MonitoringImplementationId::create(
                    std::move(slot.context_view_implementation_ids_[index])),
                coverage_start[index], coverage_end[index], completeness[index],
                std::span<const MonitoringRawCaptureEvidence::ContextAcquisition>{
                    slot.result_.raw_.context_}.subspan(begin, end - begin));
        }
        return true;
    }
    [[nodiscard]] static auto context_workers(
        const PairwisePreparedClassifierScratch& scratch) noexcept
        -> std::span<const core::WorkerId> { return scratch.context_workers_; }
    [[nodiscard]] static auto context_views(
        const PairwisePreparedClassifierScratch& scratch) noexcept
        -> std::span<const ContextSwitchWorkerAcquisitionView> {
        return scratch.context_views_;
    }
    [[nodiscard]] static auto page_before(
        const PairwisePreparedCaptureSlot& slot) noexcept
        -> std::span<const MonitoringRawCaptureEvidence::PageAcquisition> {
        return slot.result_.raw_.page_before_;
    }
    [[nodiscard]] static auto page_after(
        const PairwisePreparedCaptureSlot& slot) noexcept
        -> std::span<const MonitoringRawCaptureEvidence::PageAcquisition> {
        return slot.result_.raw_.page_after_;
    }
    [[nodiscard]] static auto append_context_evidence(
        PairwisePreparedCaptureSlot& slot, core::WorkerId worker,
        MonitoringSignalState state, std::optional<ContextSwitchCount> voluntary,
        std::optional<ContextSwitchCount> involuntary) noexcept -> bool {
        if (slot.context_evidence_storage_.size() ==
            slot.context_evidence_storage_.capacity())
            return false;
        slot.context_evidence_storage_.push_back(
            WorkerContextSwitchEvidence{worker, state, voluntary, involuntary});
        return true;
    }
    [[nodiscard]] static auto append_frequency_evidence(
        PairwisePreparedCaptureSlot& slot, FrequencyDomainId domain,
        FrequencySourceKind kind, MonitoringSignalState state,
        std::optional<FrequencyHz> minimum, std::optional<FrequencyHz> maximum,
        std::optional<FrequencyRangeHz> range,
        std::optional<FrequencyVariationRatio> variation,
        std::size_t count) noexcept -> bool {
        if (slot.frequency_evidence_storage_.size() ==
            slot.frequency_evidence_storage_.capacity())
            return false;
        slot.frequency_evidence_storage_.push_back(FrequencyDomainEvidence{
            std::move(domain), kind, state, minimum, maximum, range, variation,
            count});
        return true;
    }
    [[nodiscard]] static auto append_temperature_acquisition(
        PairwisePreparedCaptureSlot& slot, std::size_t ordinal,
        platform_linux::RawMonotonicTimestamp collection_start,
        platform_linux::RawMonotonicTimestamp collection_end,
        std::int64_t millicelsius) noexcept -> bool {
        if (ordinal >= slot.temperature_authored_.size() ||
            slot.temperature_authored_[ordinal] ||
            slot.result_.raw_.temperature_.size() ==
                slot.result_.raw_.temperature_.capacity() ||
            collection_end < collection_start)
            return false;
        auto implementation = MonitoringImplementationId::create(
            std::move(slot.temperature_implementation_ids_[ordinal]));
        slot.result_.raw_.temperature_.push_back(
            MonitoringRawCaptureEvidence::TemperatureAcquisition{
                MonitoringChannel::temperature, std::move(implementation),
                MonitoringSamplePhase::during_observation, collection_start,
                collection_end,
                core::Duration::from_base_units(
                    collection_end.nanoseconds() -
                    collection_start.nanoseconds()),
                MonitoringPayloadResult<TemperatureSnapshot>::available(
                    TemperatureSnapshot::create(
                        TemperatureSensorKind::cpu_control,
                        TemperatureMilliCelsius::from_millicelsius(
                            millicelsius)))});
        slot.temperature_authored_[ordinal] = true;
        return true;
    }
    [[nodiscard]] static auto append_frequency_acquisition(
        PairwisePreparedCaptureSlot& slot, std::size_t domain,
        std::size_t ordinal, std::span<const core::LinuxCpuId> cpus,
        FrequencySourceKind kind,
        platform_linux::RawMonotonicTimestamp collection_start,
        platform_linux::RawMonotonicTimestamp collection_end,
        std::uint64_t frequency_hz) noexcept -> bool {
        if (domain >= slot.frequency_domain_count_ ||
            ordinal >= slot.round_capacity_ ||
            collection_end < collection_start)
            return false;
        const auto authored = domain * slot.round_capacity_ + ordinal;
        if (authored >= slot.frequency_authored_.size() ||
            slot.frequency_authored_[authored] ||
            slot.result_.raw_.frequency_storage_[domain].size() ==
                slot.result_.raw_.frequency_storage_[domain].capacity())
            return false;
        if (domain >= slot.frequency_domain_offsets_.size() ||
            ordinal >= slot.frequency_membership_arenas_.size() ||
            cpus.size() != slot.frequency_domain_sizes_[domain])
            return false;
        auto& membership = *slot.frequency_membership_arenas_[ordinal];
        const auto offset = slot.frequency_domain_offsets_[domain];
        if (offset > membership.size() || cpus.size() > membership.size() - offset)
            return false;
        std::ranges::copy(cpus, membership.begin() +
            static_cast<std::ptrdiff_t>(offset));
        auto implementation = MonitoringImplementationId::create(
            std::move(slot.frequency_implementation_ids_[authored]));
        auto domain_id = FrequencyDomainId{
            slot.frequency_membership_arenas_[ordinal], offset, cpus.size()};
        slot.result_.raw_.frequency_storage_[domain].push_back(
            FrequencyAcquisition{
                MonitoringChannel::frequency, std::move(implementation),
                MonitoringSamplePhase::during_observation, collection_start,
                collection_end,
                core::Duration::from_base_units(
                    collection_end.nanoseconds() -
                    collection_start.nanoseconds()),
                MonitoringPayloadResult<FrequencySnapshot>::available(
                    FrequencySnapshot{
                        std::move(domain_id), kind,
                        FrequencyHz::from_hz(frequency_hz)})});
        slot.frequency_authored_[authored] = true;
        return true;
    }
    [[nodiscard]] static auto commit_round(
        PairwisePreparedCaptureSlot& slot, std::size_t ordinal,
        bool temperature, bool frequency) noexcept -> bool {
        if (ordinal >= slot.committed_rounds_.size() ||
            slot.committed_rounds_[ordinal])
            return false;
        if (temperature && (ordinal >= slot.temperature_authored_.size() ||
                            !slot.temperature_authored_[ordinal]))
            return false;
        if (frequency)
            for (std::size_t domain{}; domain < slot.frequency_domain_count_;
                 ++domain)
                if (!slot.frequency_authored_[
                        domain * slot.round_capacity_ + ordinal])
                    return false;
        slot.committed_rounds_[ordinal] = true;
        return true;
    }
    [[nodiscard]] static auto committed_round_count(
        const PairwisePreparedCaptureSlot& slot) noexcept -> std::size_t {
        return static_cast<std::size_t>(
            std::ranges::count(slot.committed_rounds_, true));
    }
    [[nodiscard]] static auto prepare_temperature_views(
        PairwisePreparedClassifierScratch& scratch,
        const PairwisePreparedCaptureSlot& slot) noexcept -> bool {
        if (slot.result_.raw_.temperature_.size() >
            scratch.temperature_views_.size())
            return false;
        std::ranges::fill(scratch.temperature_views_, nullptr);
        for (std::size_t index{};
             index < slot.result_.raw_.temperature_.size(); ++index)
            scratch.temperature_views_[index] =
                &slot.result_.raw_.temperature_[index];
        return true;
    }
    [[nodiscard]] static auto temperature_view_scratch(
        PairwisePreparedClassifierScratch& scratch) noexcept
        -> std::span<const MonitoringRawCaptureEvidence::TemperatureAcquisition*> {
        return scratch.temperature_views_;
    }
    [[nodiscard]] static auto frequency_view_scratch(
        PairwisePreparedClassifierScratch& scratch) noexcept
        -> std::span<const FrequencyAcquisition*> {
        return scratch.frequency_views_;
    }
    [[nodiscard]] static auto prepare_frequency_classifier_domains(
        PairwisePreparedClassifierScratch& scratch,
        std::span<const FrequencyDomainAcquisitions> domains) noexcept -> bool {
        if (domains.size() > scratch.frequency_domains_.capacity()) return false;
        // Only private prepared domains use shared CPU storage. Ordinary owning
        // domains are not accepted here because copying their CPU vector could allocate.
        for (const auto& domain : domains)
            if (!domain.domain_id.shared_cpus_ || !domain.domain_id.cpus_.empty()) return false;
        scratch.frequency_domains_.clear();
        for (const auto& domain : domains) scratch.frequency_domains_.push_back(domain);
        return true;
    }
    [[nodiscard]] static auto frequency_classifier_domains(
        PairwisePreparedClassifierScratch& scratch) noexcept
        -> std::span<FrequencyDomainAcquisitions> {
        return scratch.frequency_domains_;
    }
    [[nodiscard]] static auto frequency_result_scratch(
        PairwisePreparedClassifierScratch& scratch) noexcept
        -> std::span<std::optional<FrequencyDomainClassification>> {
        return std::span{scratch.frequency_results_}.first(
            scratch.frequency_result_count_);
    }
    [[nodiscard]] static auto finalize_page_evidence(
        PairwisePreparedCaptureSlot& slot, MonitoringSignalState state) noexcept
        -> bool {
        if (slot.result_.evidence_.page_)
            return false;
        slot.result_.evidence_.page_ = PageFaultObservationEvidence{
            state, std::move(slot.page_evidence_storage_)};
        slot.result_.evidence_.states_[0] = state;
        return true;
    }
    [[nodiscard]] static auto finalize_context_evidence(
        PairwisePreparedCaptureSlot& slot, MonitoringSignalState state) noexcept
        -> bool {
        if (slot.result_.evidence_.context_)
            return false;
        slot.result_.evidence_.context_ = ContextSwitchObservationEvidence{
            state, std::move(slot.context_evidence_storage_)};
        slot.result_.evidence_.states_[1] = state;
        return true;
    }
    [[nodiscard]] static auto finalize_temperature_evidence(
        PairwisePreparedCaptureSlot& slot,
        const TemperatureClassification& value, core::Duration interval,
        TemperatureCaptureCompleteness completeness) noexcept -> bool {
        if (slot.result_.evidence_.temperature_)
            return false;
        slot.result_.evidence_.temperature_ = TemperatureObservationEvidence{
            value.state, value.minimum, value.maximum, value.range,
            value.count, interval, completeness};
        slot.result_.evidence_.states_[2] = value.state;
        return true;
    }
    [[nodiscard]] static auto prepare_frequency_public_domain(
        PairwisePreparedCaptureSlot& slot, std::size_t domain,
        std::span<const core::LinuxCpuId> membership, FrequencySourceKind kind,
        std::string_view driver, std::string_view governor,
        std::uint64_t minimum_hz, std::uint64_t maximum_hz) noexcept -> bool {
        if (domain >= slot.frequency_domain_count_ ||
            slot.result_.raw_.frequency_views_.size() != domain ||
            slot.result_.raw_.frequency_metadata_.size() != domain)
            return false;
        auto fill = [&](std::size_t offset) noexcept
            -> std::optional<FrequencyDomainId> {
            if (domain >= slot.frequency_domain_offsets_.size() ||
                offset >= slot.frequency_membership_arenas_.size() ||
                membership.size() != slot.frequency_domain_sizes_[domain])
                return std::nullopt;
            auto& cpus = *slot.frequency_membership_arenas_[offset];
            const auto begin = slot.frequency_domain_offsets_[domain];
            if (begin > cpus.size() || membership.size() > cpus.size() - begin)
                return std::nullopt;
            std::ranges::copy(membership, cpus.begin() +
                static_cast<std::ptrdiff_t>(begin));
            return FrequencyDomainId{
                slot.frequency_membership_arenas_[offset], begin,
                membership.size()};
        };
        auto view_domain = fill(slot.round_capacity_);
        auto metadata_domain = fill(slot.round_capacity_ + 1U);
        if (!view_domain || !metadata_domain)
            return false;
        auto& tokens = slot.frequency_token_storage_[domain];
        if (driver.size() > tokens[0].capacity() ||
            governor.size() > tokens[1].capacity())
            return false;
        tokens[0].assign(driver);
        tokens[1].assign(governor);
        slot.result_.raw_.frequency_views_.push_back(
            FrequencyDomainAcquisitions{
                std::move(*view_domain),
                slot.result_.raw_.frequency_storage_[domain]});
        slot.result_.raw_.frequency_metadata_.push_back(
            FrequencyPolicyMetadata{
                std::move(*metadata_domain), kind,
                FrequencyDriverId{std::move(tokens[0])},
                FrequencyGovernorId{std::move(tokens[1])},
                FrequencyHz::from_hz(minimum_hz),
                FrequencyHz::from_hz(maximum_hz)});
        return true;
    }
    [[nodiscard]] static auto initialize_frequency_implementation_ids(
        PairwisePreparedCaptureSlots& slots, std::size_t domain,
        FrequencySourceKind kind) noexcept -> bool {
        for (auto& optional : slots.slots_) {
            if (!optional)
                return false;
            auto& slot = *optional;
            if (domain >= slot.frequency_domain_count_)
                return false;
            const auto identifier = kind ==
                    FrequencySourceKind::hardware_current
                ? std::string_view{"linux-cpufreq-cpuinfo-cur-freq"}
                : std::string_view{"linux-cpufreq-scaling-cur-freq"};
            for (std::size_t ordinal{}; ordinal < slot.round_capacity_;
                 ++ordinal) {
                auto& destination = slot.frequency_implementation_ids_[
                    domain * slot.round_capacity_ + ordinal];
                if (identifier.size() > destination.capacity())
                    return false;
                destination.assign(identifier);
            }
        }
        return true;
    }
    [[nodiscard]] static auto append_frequency_classification(
        PairwisePreparedCaptureSlot& slot, std::size_t domain,
        const FrequencyDomainClassification& value,
        std::span<const core::LinuxCpuId> membership) noexcept -> bool {
        if (domain >= slot.frequency_domain_count_ ||
            slot.frequency_evidence_storage_.size() != domain)
            return false;
        const auto arena_index = slot.round_capacity_ + 2U;
        if (domain >= slot.frequency_domain_offsets_.size() ||
            arena_index >= slot.frequency_membership_arenas_.size() ||
            membership.size() != slot.frequency_domain_sizes_[domain])
            return false;
        auto& cpus = *slot.frequency_membership_arenas_[arena_index];
        const auto offset = slot.frequency_domain_offsets_[domain];
        if (offset > cpus.size() || membership.size() > cpus.size() - offset)
            return false;
        std::ranges::copy(membership, cpus.begin() +
            static_cast<std::ptrdiff_t>(offset));
        slot.frequency_evidence_storage_.push_back(FrequencyDomainEvidence{
            FrequencyDomainId{slot.frequency_membership_arenas_[arena_index],
                              offset, membership.size()},
            value.kind, value.state,
            value.minimum, value.maximum, value.range, value.variation,
            value.count});
        return true;
    }
    static auto set_channel_state(PairwisePreparedCaptureSlot& slot,
                                  MonitoringChannel channel,
                                  MonitoringSignalState state) noexcept -> void {
        const auto index = channel == MonitoringChannel::page_faults ? 0U
            : channel == MonitoringChannel::context_switches ? 1U
            : channel == MonitoringChannel::temperature ? 2U
                                                       : 3U;
        slot.result_.evidence_.states_[index] = state;
    }
    static auto increment_scheduled(PairwisePreparedCaptureSlot& slot) noexcept
        -> void { ++slot.result_.activity_.scheduled_; }
    static auto increment_completed(PairwisePreparedCaptureSlot& slot) noexcept
        -> void { ++slot.result_.activity_.completed_; }
    static auto increment_missed(PairwisePreparedCaptureSlot& slot) noexcept
        -> void { ++slot.result_.activity_.missed_; }
    static auto increment_wakeup(PairwisePreparedCaptureSlot& slot) noexcept
        -> void { ++slot.result_.activity_.wakeups_; }
    static auto increment_context_drain(
        PairwisePreparedCaptureSlot& slot) noexcept -> void {
        ++slot.result_.activity_.drains_;
    }
    static auto increment_temperature(
        PairwisePreparedCaptureSlot& slot) noexcept -> void {
        ++slot.result_.activity_.temperature_;
    }
    static auto increment_frequency(
        PairwisePreparedCaptureSlot& slot) noexcept -> void {
        ++slot.result_.activity_.frequency_;
    }
    [[nodiscard]] static auto raw_temperature(
        const PairwisePreparedCaptureSlot& slot) noexcept
        -> std::span<const MonitoringRawCaptureEvidence::TemperatureAcquisition> {
        return slot.result_.raw_.temperature_;
    }
    [[nodiscard]] static auto raw_frequency(
        const PairwisePreparedCaptureSlot& slot) noexcept
        -> std::span<const FrequencyDomainAcquisitions> {
        return slot.result_.raw_.frequency_views_;
    }
    [[nodiscard]] static auto missed_rounds(
        const PairwisePreparedCaptureSlot& slot) noexcept -> std::uint64_t {
        return slot.result_.activity_.missed_;
    }
    [[nodiscard]] static auto finalize_frequency_evidence(
        PairwisePreparedCaptureSlot& slot, MonitoringSignalState state,
        core::Duration interval, FrequencyCaptureCompleteness completeness) noexcept
        -> bool {
        if (slot.result_.evidence_.frequency_)
            return false;
        slot.result_.evidence_.frequency_ = FrequencyObservationEvidence{
            state, std::move(slot.frequency_evidence_storage_), interval,
            completeness};
        slot.result_.evidence_.states_[3] = state;
        return true;
    }
    [[nodiscard]] static auto prepare_slots(
        const PairwisePreparedCaptureDimensions& dimensions,
        std::size_t frequency_domain_capacity,
        std::size_t frequency_membership_capacity)
        -> PairwisePreparedCaptureSlots {
        std::vector<std::optional<PairwisePreparedCaptureSlot>> slots;
        slots.reserve(dimensions.captures);
        for (std::size_t index{}; index < dimensions.captures; ++index)
            slots.emplace_back(prepare_slot(
                dimensions, frequency_domain_capacity,
                frequency_membership_capacity));
        return PairwisePreparedCaptureSlots{std::move(slots)};
    }

    [[nodiscard]] static auto prepare_scratch(
        const PairwisePreparedCaptureDimensions& dimensions,
        std::size_t frequency_domains) -> PairwisePreparedClassifierScratch {
        PairwisePreparedClassifierScratch scratch;
        const auto page_workers = dimensions.page ? dimensions.workers : 0U;
        const auto context_workers = dimensions.context ? dimensions.workers : 0U;
        scratch.page_pairs_.reserve(page_workers);
        scratch.context_workers_.reserve(context_workers);
        scratch.context_views_.reserve(context_workers);
        scratch.context_membership_.reserve(context_workers);
        scratch.context_lookup_.resize(context_workers);
        scratch.temperature_views_.resize(
            dimensions.temperature ? dimensions.rounds : 0U);
        scratch.frequency_views_.resize(
            dimensions.frequency ? dimensions.rounds : 0U);
        scratch.frequency_results_.resize(
            dimensions.frequency ? frequency_domains : 0U);
        scratch.frequency_domains_.reserve(
            dimensions.frequency ? frequency_domains : 0U);
        return scratch;
    }

    [[nodiscard]] static auto commit_frequency_partition(
        PairwisePreparedCaptureSlots& slots,
        PairwisePreparedClassifierScratch& scratch,
        std::span<const std::size_t> memberships) noexcept -> bool {
        const auto domains = memberships.size();
        if (scratch.frequency_partition_committed_ ||
            domains > scratch.frequency_results_.size() ||
            domains > scratch.frequency_domains_.capacity())
            return false;
        std::size_t total{};
        for (const auto membership : memberships) {
            if (membership == 0U ||
                membership > std::numeric_limits<std::size_t>::max() - total)
                return false;
            total += membership;
        }
        if (domains != 0U && !slots.slots_.empty()) {
            const auto& first = slots.slots_.front();
            if (!first || (first->round_capacity_ != 0U &&
                           domains > std::numeric_limits<std::size_t>::max() /
                               first->round_capacity_))
                return false;
        }
        for (const auto& optional : slots.slots_) {
            if (!optional || optional->frequency_partition_committed_ ||
                domains > optional->frequency_domain_offsets_.size() ||
                domains > optional->frequency_domain_sizes_.size() ||
                domains > optional->result_.raw_.frequency_storage_.size() ||
                domains > optional->frequency_token_storage_.size() ||
                domains > optional->frequency_evidence_storage_.capacity() ||
                domains > optional->result_.raw_.frequency_views_.capacity() ||
                domains > optional->result_.raw_.frequency_metadata_.capacity() ||
                domains * optional->round_capacity_ >
                    optional->frequency_authored_.size() ||
                domains * optional->round_capacity_ >
                    optional->frequency_implementation_ids_.size())
                return false;
            if (optional->frequency_membership_arenas_.empty()) {
                if (total != 0U)
                    return false;
            } else if (!std::ranges::all_of(
                           optional->frequency_membership_arenas_,
                           [&](const auto& arena) {
                               return arena && arena->size() >= total;
                           })) {
                return false;
            }
        }
        scratch.frequency_result_count_ = domains;
        scratch.frequency_partition_committed_ = true;
        for (auto& optional : slots.slots_) {
            auto& slot = *optional;
            slot.frequency_domain_count_ = domains;
            slot.frequency_offset_count_ = domains;
            slot.frequency_size_count_ = domains;
            slot.frequency_storage_count_ = domains;
            slot.frequency_token_count_ = domains;
            slot.frequency_authored_count_ = domains * slot.round_capacity_;
            slot.frequency_implementation_count_ =
                domains * slot.round_capacity_;
            slot.frequency_partition_committed_ = true;
            std::size_t offset{};
            for (std::size_t domain{}; domain < domains; ++domain) {
                slot.frequency_domain_offsets_[domain] = offset;
                slot.frequency_domain_sizes_[domain] = memberships[domain];
                offset += memberships[domain];
            }
        }
        return true;
    }

    [[nodiscard]] static auto take(PairwisePreparedCaptureSlots& slots,
                                   std::size_t index) noexcept
        -> std::optional<PairwisePreparedCaptureSlot> {
        if (index >= slots.slots_.size() || !slots.slots_[index])
            return std::nullopt;
        auto result = std::optional<PairwisePreparedCaptureSlot>{
            std::move(*slots.slots_[index])};
        slots.slots_[index].reset();
        return result;
    }

    [[nodiscard]] static auto context_capacity(
        const PairwisePreparedCaptureSlots& slots) noexcept -> std::size_t {
        std::size_t result{};
        for (const auto& slot : slots.slots_)
            if (slot)
                result += slot->context_capacity();
        return result;
    }
    [[nodiscard]] static auto context_offset_capacity(
        const PairwisePreparedCaptureSlots& slots) noexcept -> std::size_t {
        std::size_t result{};
        for (const auto& slot : slots.slots_)
            if (slot)
                result += slot->context_offset_capacity();
        return result;
    }
    [[nodiscard]] static auto context_event_capacity(
        const PairwisePreparedCaptureSlots& slots) noexcept -> std::size_t {
        std::size_t result{};
        for (const auto& slot : slots.slots_)
            if (slot)
                result += slot->context_event_capacity();
        return result;
    }
    [[nodiscard]] static auto frequency_nested_cpu_capacity(
        const PairwisePreparedCaptureSlots& slots) noexcept -> std::size_t {
        std::size_t result{};
        for (const auto& slot : slots.slots_)
            if (slot)
                result += slot->frequency_nested_cpu_capacity();
        return result;
    }
    [[nodiscard]] static auto frequency_layout(
        const PairwisePreparedCaptureSlots& slots,
        const PairwisePreparedClassifierScratch& scratch) noexcept
        -> PairwisePreparedFrequencyLayoutObservation {
        if (slots.slots_.empty() || !slots.slots_.front())
            return {.result_count = scratch.frequency_result_count_,
                    .committed = scratch.frequency_partition_committed_};
        const auto& slot = *slots.slots_.front();
        return {
            .domain_capacity = slot.frequency_domain_offsets_.size(),
            .membership_capacity = slot.frequency_membership_arenas_.empty()
                ? 0U
                : slot.frequency_membership_arenas_.front()->size(),
            .domain_count = slot.frequency_domain_count_,
            .offset_count = slot.frequency_offset_count_,
            .size_count = slot.frequency_size_count_,
            .storage_count = slot.frequency_storage_count_,
            .token_count = slot.frequency_token_count_,
            .authored_count = slot.frequency_authored_count_,
            .implementation_count = slot.frequency_implementation_count_,
            .result_count = scratch.frequency_result_count_,
            .committed = slot.frequency_partition_committed_ &&
                scratch.frequency_partition_committed_,
        };
    }

  private:
    [[nodiscard]] static auto prepare_slot(
        const PairwisePreparedCaptureDimensions& value,
        std::size_t frequency_domain_capacity,
        std::size_t frequency_membership_capacity)
        -> PairwisePreparedCaptureSlot {
        PairwisePreparedCaptureSlot slot;
        const auto page_workers = value.page ? value.workers : 0U;
        const auto context_acquisitions =
            value.context ? value.workers * (value.rounds + 1U) : 0U;
        slot.context_event_capacity_ =
            context_acquisitions * value.events_per_drain;
        const auto temperature_acquisitions =
            value.temperature ? value.rounds : 0U;
        const auto frequency_domains =
            value.frequency ? frequency_domain_capacity : 0U;

        slot.result_.raw_.page_before_.reserve(page_workers);
        slot.result_.raw_.page_after_.reserve(page_workers);
        slot.page_implementation_ids_.reserve(page_workers * 2U);
        for (std::size_t index{}; index < page_workers * 2U; ++index)
            slot.page_implementation_ids_.emplace_back("linux-proc-task-stat");
        slot.result_.raw_.context_.reserve(context_acquisitions);
        slot.context_authored_.resize(context_acquisitions);
        slot.result_.raw_.context_worker_offsets_.resize(
            value.context ? value.workers + 1U : 0U);
        if (value.context) {
            const auto stride = value.rounds + 1U;
            for (std::size_t worker{}; worker < value.workers; ++worker) {
                slot.result_.raw_.context_worker_offsets_[worker] =
                    worker * stride;
                for (std::size_t ordinal{}; ordinal < stride; ++ordinal) {
                    std::vector<ContextSwitchEvent> events;
                    events.reserve(value.events_per_drain);
                    slot.result_.raw_.context_.push_back(
                        MonitoringRawCaptureEvidence::ContextAcquisition{
                            MonitoringChannel::context_switches,
                            MonitoringImplementationId::create(
                                "linux-perf-context-switch-records"),
                            MonitoringSamplePhase::during_observation,
                            platform_linux::RawMonotonicTimestamp::from_nanoseconds(0U),
                            platform_linux::RawMonotonicTimestamp::from_nanoseconds(0U),
                            core::Duration::from_base_units(0U),
                            MonitoringPayloadResult<ContextSwitchEventBatch>::available(
                                ContextSwitchEventBatch::create(
                                    core::WorkerId{0U},
                                    ContextSwitchCaptureCompleteness::complete,
                                    std::move(events)))});
                }
            }
            slot.result_.raw_.context_worker_offsets_[value.workers] =
                value.workers * stride;
        }
        slot.result_.raw_.temperature_.reserve(temperature_acquisitions);
        slot.temperature_authored_.resize(temperature_acquisitions);
        slot.temperature_implementation_ids_.reserve(temperature_acquisitions);
        for (std::size_t index{}; index < temperature_acquisitions; ++index)
            slot.temperature_implementation_ids_.emplace_back(
                "linux-hwmon-k10temp-tctl");
        slot.result_.raw_.frequency_storage_.resize(frequency_domains);
        for (auto& acquisitions : slot.result_.raw_.frequency_storage_)
            acquisitions.reserve(value.rounds);
        slot.result_.raw_.frequency_views_.reserve(frequency_domains);
        slot.result_.raw_.frequency_metadata_.reserve(frequency_domains);
        slot.page_evidence_storage_.reserve(page_workers);
        slot.context_evidence_storage_.reserve(
            value.context ? value.workers : 0U);
        slot.context_view_implementation_ids_.reserve(
            value.context ? value.workers : 0U);
        for (std::size_t index{}; index < (value.context ? value.workers : 0U);
             ++index)
            slot.context_view_implementation_ids_.emplace_back(
                "linux-perf-context-switch-records");
        slot.frequency_evidence_storage_.reserve(frequency_domains);

        if (value.frequency) {
            slot.frequency_membership_arenas_.reserve(value.rounds + 3U);
            for (std::size_t copy{}; copy < value.rounds + 3U; ++copy)
                slot.frequency_membership_arenas_.push_back(
                    std::make_shared<std::vector<core::LinuxCpuId>>(
                        frequency_membership_capacity, core::LinuxCpuId{0U}));
        }
        slot.frequency_domain_offsets_.resize(frequency_domains);
        slot.frequency_domain_sizes_.resize(frequency_domains);
        slot.frequency_token_storage_.resize(frequency_domains);
        for (auto& tokens : slot.frequency_token_storage_) {
            tokens[0].reserve(value.base_page_bytes);
            tokens[1].reserve(value.base_page_bytes);
        }
        slot.round_capacity_ = value.rounds;
        slot.frequency_authored_.resize(frequency_domains * value.rounds);
        slot.frequency_implementation_ids_.resize(
            frequency_domains * value.rounds);
        for (auto& identifier : slot.frequency_implementation_ids_)
            identifier.reserve(value.base_page_bytes);
        slot.committed_rounds_.resize(
            (value.context || value.temperature || value.frequency)
                ? value.rounds
                : 0U);
        return slot;
    }
};

} // namespace epycbench::monitoring::detail
