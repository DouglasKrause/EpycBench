#include "epycbench/monitoring/frequency.hpp"

#include "frequency_classifier_access.hpp"
#include "frequency_classifier_kernel.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace epycbench::monitoring {
namespace {

auto frequency_error(std::string_view code, std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Frequency classification input is invalid",
            .operation = "classify_frequency",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto source_kind(platform_linux::CpuFrequencyCurrentKind kind) noexcept -> FrequencySourceKind {
    return kind == platform_linux::CpuFrequencyCurrentKind::hardware_current
               ? FrequencySourceKind::hardware_current
               : FrequencySourceKind::scaling_current;
}

auto production_read(void* context) noexcept -> std::expected<std::uint64_t, core::Error> {
    return static_cast<platform_linux::CpuFrequencyPolicySource*>(context)->read();
}

auto implementation_id(FrequencySourceKind kind) -> MonitoringImplementationId {
    return MonitoringImplementationId::create(
        kind == FrequencySourceKind::hardware_current
            ? "linux-cpufreq-cpuinfo-cur-freq"
            : "linux-cpufreq-scaling-cur-freq");
}

auto source_kind(const MonitoringImplementationId& id) noexcept
    -> std::optional<FrequencySourceKind> {
    if (id.value() == "linux-cpufreq-cpuinfo-cur-freq") {
        return FrequencySourceKind::hardware_current;
    }
    if (id.value() == "linux-cpufreq-scaling-cur-freq") {
        return FrequencySourceKind::scaling_current;
    }
    return std::nullopt;
}

} // namespace

FrequencyDomainId::FrequencyDomainId(std::vector<core::LinuxCpuId> cpus) noexcept
    : cpus_{std::move(cpus)} {}
FrequencyDomainId::FrequencyDomainId(
    std::shared_ptr<const std::vector<core::LinuxCpuId>> storage,
    std::size_t offset, std::size_t count) noexcept
    : shared_cpus_{std::move(storage)}, offset_{offset}, count_{count} {}
auto FrequencyDomainId::create(std::vector<core::LinuxCpuId> cpus)
    -> std::expected<FrequencyDomainId, core::Error> {
    if (cpus.empty() || !std::ranges::is_sorted(cpus) ||
        std::adjacent_find(cpus.begin(), cpus.end()) != cpus.end()) {
        return std::unexpected{
            frequency_error("monitoring.frequency.invalid_input", "domain")};
    }
    return FrequencyDomainId{std::move(cpus)};
}
auto FrequencyDomainId::cpus() const noexcept -> std::span<const core::LinuxCpuId> {
    return shared_cpus_
        ? std::span<const core::LinuxCpuId>{*shared_cpus_}.subspan(offset_, count_)
        : std::span<const core::LinuxCpuId>{cpus_};
}
auto FrequencyDomainId::operator<=>(const FrequencyDomainId& other) const noexcept
    -> std::strong_ordering {
    return std::lexicographical_compare_three_way(
        cpus().begin(), cpus().end(), other.cpus().begin(), other.cpus().end());
}
auto FrequencyDomainId::operator==(const FrequencyDomainId& other) const noexcept
    -> bool {
    return std::ranges::equal(cpus(), other.cpus());
}

FrequencyVariationRatio::FrequencyVariationRatio(std::uint64_t numerator,
                                                 std::uint64_t denominator) noexcept
    : numerator_{numerator}, denominator_{denominator} {}
auto FrequencyVariationRatio::create(std::uint64_t numerator, std::uint64_t denominator)
    -> std::expected<FrequencyVariationRatio, core::Error> {
    if (denominator == 0U || numerator > denominator) {
        return std::unexpected{
            frequency_error("monitoring.frequency.invalid_input", "ratio")};
    }
    return FrequencyVariationRatio{numerator, denominator};
}
auto FrequencyVariationRatio::numerator() const noexcept -> std::uint64_t { return numerator_; }
auto FrequencyVariationRatio::denominator() const noexcept -> std::uint64_t {
    return denominator_;
}

FrequencyDriverId::FrequencyDriverId(std::string value) noexcept : value_{std::move(value)} {}
auto FrequencyDriverId::create(std::string value) -> FrequencyDriverId {
    return FrequencyDriverId{std::move(value)};
}
auto FrequencyDriverId::value() const noexcept -> std::string_view { return value_; }
FrequencyGovernorId::FrequencyGovernorId(std::string value) noexcept : value_{std::move(value)} {}
auto FrequencyGovernorId::create(std::string value) -> FrequencyGovernorId {
    return FrequencyGovernorId{std::move(value)};
}
auto FrequencyGovernorId::value() const noexcept -> std::string_view { return value_; }

FrequencyPolicyMetadata::FrequencyPolicyMetadata(
    FrequencyDomainId domain_id, FrequencySourceKind kind, FrequencyDriverId driver,
    FrequencyGovernorId governor, FrequencyHz minimum, FrequencyHz maximum) noexcept
    : domain_id_{std::move(domain_id)}, kind_{kind}, driver_{std::move(driver)},
      governor_{std::move(governor)}, minimum_{minimum}, maximum_{maximum} {}
auto FrequencyPolicyMetadata::create(FrequencyDomainId domain_id, FrequencySourceKind kind,
                                     FrequencyDriverId driver, FrequencyGovernorId governor,
                                     FrequencyHz minimum, FrequencyHz maximum)
    -> FrequencyPolicyMetadata {
    return {std::move(domain_id), kind, std::move(driver), std::move(governor), minimum, maximum};
}
auto FrequencyPolicyMetadata::domain_id() const noexcept -> const FrequencyDomainId& {
    return domain_id_;
}
auto FrequencyPolicyMetadata::source_kind() const noexcept -> FrequencySourceKind { return kind_; }
auto FrequencyPolicyMetadata::scaling_driver() const noexcept -> const FrequencyDriverId& {
    return driver_;
}
auto FrequencyPolicyMetadata::scaling_governor() const noexcept -> const FrequencyGovernorId& {
    return governor_;
}
auto FrequencyPolicyMetadata::scaling_minimum() const noexcept -> FrequencyHz { return minimum_; }
auto FrequencyPolicyMetadata::scaling_maximum() const noexcept -> FrequencyHz { return maximum_; }

FrequencySnapshot::FrequencySnapshot(FrequencyDomainId domain_id, FrequencySourceKind kind,
                                     FrequencyHz frequency) noexcept
    : domain_id_{std::move(domain_id)}, kind_{kind}, frequency_{frequency} {}
auto FrequencySnapshot::create(FrequencyDomainId domain_id, FrequencySourceKind kind,
                               FrequencyHz frequency) -> FrequencySnapshot {
    return {std::move(domain_id), kind, frequency};
}
auto FrequencySnapshot::domain_id() const noexcept -> const FrequencyDomainId& { return domain_id_; }
auto FrequencySnapshot::source_kind() const noexcept -> FrequencySourceKind { return kind_; }
auto FrequencySnapshot::frequency() const noexcept -> FrequencyHz { return frequency_; }

FrequencySource::FrequencySource(FrequencyPolicyMetadata metadata, void* context,
                                 ReadOperation read,
                                 std::unique_ptr<platform_linux::CpuFrequencyPolicySource> source) noexcept
    : metadata_{std::move(metadata)}, context_{context}, read_{read}, source_{std::move(source)} {}
FrequencySource::FrequencySource(FrequencySource&&) noexcept = default;
auto FrequencySource::operator=(FrequencySource&&) noexcept -> FrequencySource& = default;
FrequencySource::~FrequencySource() noexcept = default;

auto FrequencySource::prepare(core::LinuxCpuId cpu)
    -> std::expected<std::optional<FrequencySource>, core::Error> {
    auto prepared = platform_linux::CpuFrequencyPolicySource::discover(cpu);
    if (!prepared) {
        return std::unexpected{std::move(prepared.error())};
    }
    if (!*prepared) {
        return std::optional<FrequencySource>{};
    }
    auto domain = FrequencyDomainId::create(
        std::vector<core::LinuxCpuId>{(**prepared).related_cpus().begin(),
                                      (**prepared).related_cpus().end()});
    if (!domain) {
        return std::unexpected{std::move(domain.error())};
    }
    const auto kind = source_kind((**prepared).kind());
    auto metadata = FrequencyPolicyMetadata::create(
        std::move(*domain), kind,
        FrequencyDriverId::create((**prepared).scaling_driver()),
        FrequencyGovernorId::create((**prepared).scaling_governor()),
        FrequencyHz::from_hz((**prepared).scaling_minimum_hz()),
        FrequencyHz::from_hz((**prepared).scaling_maximum_hz()));
    auto owned = std::make_unique<platform_linux::CpuFrequencyPolicySource>(std::move(**prepared));
    auto* context = owned.get();
    return std::optional<FrequencySource>{
        FrequencySource{std::move(metadata), context, production_read, std::move(owned)}};
}
auto FrequencySource::support_callback(void*) noexcept -> MonitoringSourceSupport {
    return MonitoringSourceSupport::supported;
}
auto FrequencySource::read_callback(void* context) noexcept
    -> std::expected<MonitoringPayloadResult<FrequencySnapshot>, core::Error> {
    auto& source = *static_cast<FrequencySource*>(context);
    auto value = source.read_(source.context_);
    if (!value) {
        return std::unexpected{std::move(value.error())};
    }
    return MonitoringPayloadResult<FrequencySnapshot>::available(FrequencySnapshot::create(
        source.metadata_.domain_id(), source.metadata_.source_kind(), FrequencyHz::from_hz(*value)));
}
auto FrequencySource::operations() noexcept
    -> std::expected<MonitoringSourceOperations<FrequencySnapshot>, core::Error> {
    return MonitoringSourceOperations<FrequencySnapshot>::create(this, support_callback,
                                                                 read_callback);
}
auto FrequencySource::implementation_id() const -> MonitoringImplementationId {
    return monitoring::implementation_id(metadata_.source_kind());
}
auto FrequencySource::metadata() const noexcept -> const FrequencyPolicyMetadata& {
    return metadata_;
}

FrequencyDomainEvidence::FrequencyDomainEvidence(
    FrequencyDomainId domain_id, FrequencySourceKind kind, MonitoringSignalState state,
    std::optional<FrequencyHz> minimum, std::optional<FrequencyHz> maximum,
    std::optional<FrequencyRangeHz> range, std::optional<FrequencyVariationRatio> variation,
    std::size_t count) noexcept
    : domain_id_{std::move(domain_id)}, kind_{kind}, state_{state}, minimum_{minimum},
      maximum_{maximum}, range_{range}, variation_{variation}, count_{count} {}
auto FrequencyDomainEvidence::create(
    FrequencyDomainId domain_id, FrequencySourceKind kind, MonitoringSignalState state,
    std::optional<FrequencyHz> minimum, std::optional<FrequencyHz> maximum,
    std::optional<FrequencyRangeHz> range, std::optional<FrequencyVariationRatio> variation,
    std::size_t count) -> FrequencyDomainEvidence {
    return {std::move(domain_id), kind, state, minimum, maximum, range, variation, count};
}
auto FrequencyDomainEvidence::domain_id() const noexcept -> const FrequencyDomainId& {
    return domain_id_;
}
auto FrequencyDomainEvidence::source_kind() const noexcept -> FrequencySourceKind { return kind_; }
auto FrequencyDomainEvidence::state() const noexcept -> MonitoringSignalState { return state_; }
auto FrequencyDomainEvidence::observed_minimum() const noexcept
    -> const std::optional<FrequencyHz>& { return minimum_; }
auto FrequencyDomainEvidence::observed_maximum() const noexcept
    -> const std::optional<FrequencyHz>& { return maximum_; }
auto FrequencyDomainEvidence::observed_range() const noexcept
    -> const std::optional<FrequencyRangeHz>& { return range_; }
auto FrequencyDomainEvidence::observed_variation() const noexcept
    -> const std::optional<FrequencyVariationRatio>& { return variation_; }
auto FrequencyDomainEvidence::sample_count() const noexcept -> std::size_t { return count_; }

FrequencyObservationEvidence::FrequencyObservationEvidence(
    MonitoringSignalState state, std::vector<FrequencyDomainEvidence> domains,
    core::Duration interval, FrequencyCaptureCompleteness completeness) noexcept
    : state_{state}, domains_{std::move(domains)}, interval_{interval},
      completeness_{completeness} {}
auto FrequencyObservationEvidence::state() const noexcept -> MonitoringSignalState { return state_; }
auto FrequencyObservationEvidence::domains() const noexcept
    -> std::span<const FrequencyDomainEvidence> { return domains_; }
auto FrequencyObservationEvidence::sampling_interval() const noexcept -> core::Duration {
    return interval_;
}
auto FrequencyObservationEvidence::capture_completeness() const noexcept
    -> FrequencyCaptureCompleteness { return completeness_; }

auto detail::FrequencyClassifierAccess::threshold_detected(std::uint64_t range,
                                                           std::uint64_t maximum) noexcept -> bool {
    if (maximum == 0U) {
        return false;
    }
    const auto quotient = maximum / 20U;
    const auto remainder = maximum % 20U;
    return range > quotient || (range == quotient && remainder == 0U);
}

namespace {
auto classify_domain_bounded(
    platform_linux::RawMonotonicTimestamp start,
    platform_linux::RawMonotonicTimestamp end, core::Duration interval,
    const FrequencyDomainAcquisitions& input,
    std::span<const FrequencyAcquisition*> available_scratch) noexcept
    -> std::expected<detail::FrequencyDomainClassification,
                     detail::FrequencyClassifierFailure> {
    bool unavailable = false;
    bool first = true;
    platform_linux::RawMonotonicTimestamp previous = start;
    std::size_t available_count{};
    if (available_scratch.size() < input.acquisitions.size())
        std::terminate();
    std::optional<FrequencySourceKind> kind;
    for (const auto& item : input.acquisitions) {
        if (!first && item.collection_start() < previous) {
            return std::unexpected{
                detail::FrequencyClassifierFailure::ordering};
        }
        first = false;
        previous = item.collection_start();
        if (item.channel() != MonitoringChannel::frequency ||
            item.phase() != MonitoringSamplePhase::during_observation) {
            return std::unexpected{
                detail::FrequencyClassifierFailure::metadata};
        }
        if (item.collection_start() < start || item.collection_end() > end) {
            return std::unexpected{
                detail::FrequencyClassifierFailure::boundary_overlap};
        }
        const auto envelope_kind = source_kind(item.implementation_id());
        if (!envelope_kind || (kind && *kind != *envelope_kind)) {
            return std::unexpected{
                detail::FrequencyClassifierFailure::implementation};
        }
        kind = *envelope_kind;
        if (item.status() == MonitoringAcquisitionStatus::unavailable) {
            unavailable = true;
            continue;
        }
        const auto& payload = *item.payload();
        if (payload.domain_id() != input.domain_id || payload.source_kind() != *kind) {
            return std::unexpected{
                detail::FrequencyClassifierFailure::source};
        }
        available_scratch[available_count++] = &item;
    }
    const auto available = available_scratch.first(available_count);
    if (!kind) {
        if (!unavailable) {
            return std::unexpected{
                detail::FrequencyClassifierFailure::sample_count};
        }
        return std::unexpected{
            detail::FrequencyClassifierFailure::implementation};
    }
    if (available.size() < 2U && !unavailable) {
        return std::unexpected{
            detail::FrequencyClassifierFailure::sample_count};
    }
    if (!unavailable && available.size() >= 2U) {
        const auto limit = interval.value();
        if (available.front()->collection_end().nanoseconds() - start.nanoseconds() > limit) {
            return std::unexpected{
                detail::FrequencyClassifierFailure::leading_gap};
        }
        for (std::size_t index = 1; index < available.size(); ++index) {
            if (available[index]->collection_end().nanoseconds() -
                    available[index - 1]->collection_start().nanoseconds() >
                limit) {
                return std::unexpected{
                    detail::FrequencyClassifierFailure::internal_gap};
            }
        }
        if (end.nanoseconds() - available.back()->collection_start().nanoseconds() > limit) {
            return std::unexpected{
                detail::FrequencyClassifierFailure::trailing_gap};
        }
    }

    std::optional<FrequencyHz> minimum;
    std::optional<FrequencyHz> maximum;
    std::optional<FrequencyRangeHz> range;
    std::optional<FrequencyVariationRatio> variation;
    if (!available.empty()) {
        auto min_value = available.front()->payload()->frequency().value();
        auto max_value = min_value;
        for (const auto* item : available) {
            min_value = std::min(min_value, item->payload()->frequency().value());
            max_value = std::max(max_value, item->payload()->frequency().value());
        }
        minimum = FrequencyHz::from_hz(min_value);
        maximum = FrequencyHz::from_hz(max_value);
        if (available.size() >= 2U) {
            const auto range_value = max_value - min_value;
            range = FrequencyRangeHz::from_hz(range_value);
            auto ratio = FrequencyVariationRatio::create(range_value, max_value);
            if (!ratio)
                return std::unexpected{
                    detail::FrequencyClassifierFailure::ratio};
            variation = *ratio;
        }
    }
    const auto detected = range && detail::FrequencyClassifierAccess::threshold_detected(
                                       range->value(), maximum->value());
    const auto state = detected ? MonitoringSignalState::detected
                                : unavailable ? MonitoringSignalState::unavailable
                                              : MonitoringSignalState::clear;
    return detail::FrequencyDomainClassification{
        .domain = &input.domain_id,
        .kind = *kind,
        .state = state,
        .minimum = minimum,
        .maximum = maximum,
        .range = range,
        .variation = variation,
        .count = available.size()};
}
} // namespace

auto detail::classify_frequency_bounded(
    platform_linux::RawMonotonicTimestamp start,
    platform_linux::RawMonotonicTimestamp end, core::Duration interval,
    FrequencyCaptureCompleteness completeness,
    std::span<const FrequencyDomainAcquisitions> domains,
    std::span<const FrequencyAcquisition*> available_scratch,
    std::span<std::optional<FrequencyDomainClassification>> result_scratch,
    FrequencyEvidenceSink sink) noexcept
    -> std::expected<MonitoringSignalState, FrequencyClassifierFailure> {
    if (end < start || interval.value() == 0U || domains.empty())
        return std::unexpected{FrequencyClassifierFailure::interval};
    if (completeness == FrequencyCaptureCompleteness::incomplete)
        return std::unexpected{FrequencyClassifierFailure::driver};
    if (result_scratch.size() < domains.size() || sink.append == nullptr)
        std::terminate();
    for (std::size_t index{}; index < domains.size(); ++index) {
        result_scratch[index].reset();
        if (index != 0U &&
            !(domains[index - 1U].domain_id < domains[index].domain_id))
            return std::unexpected{FrequencyClassifierFailure::domain_order};
        auto classified = classify_domain_bounded(
            start, end, interval, domains[index], available_scratch);
        if (!classified)
            return std::unexpected{classified.error()};
        result_scratch[index].emplace(std::move(*classified));
    }
    auto state = MonitoringSignalState::clear;
    for (std::size_t index{}; index < domains.size(); ++index) {
        const auto& value = *result_scratch[index];
        if (!sink.append(sink.context, value))
            std::terminate();
        if (value.state == MonitoringSignalState::detected)
            state = MonitoringSignalState::detected;
        else if (value.state == MonitoringSignalState::unavailable &&
                 state != MonitoringSignalState::detected)
            state = MonitoringSignalState::unavailable;
    }
    return state;
}

auto classify_frequency(platform_linux::RawMonotonicTimestamp start,
                        platform_linux::RawMonotonicTimestamp end, core::Duration interval,
                        FrequencyCaptureCompleteness completeness,
                        std::span<const FrequencyDomainAcquisitions> domains)
    -> std::expected<FrequencyObservationEvidence, core::Error> {
    std::vector<FrequencyDomainEvidence> evidence;
    evidence.reserve(domains.size());
    std::vector<const FrequencyAcquisition*> available;
    std::size_t maximum{};
    for(const auto& domain:domains)
        maximum=std::max(maximum,domain.acquisitions.size());
    available.resize(maximum);
    std::vector<std::optional<detail::FrequencyDomainClassification>> results(
        domains.size());
    const detail::FrequencyEvidenceSink sink{
        nullptr,
        +[](void*,const detail::FrequencyDomainClassification&) noexcept {
            return true;
        }};
    auto classified=detail::classify_frequency_bounded(
        start,end,interval,completeness,domains,available,results,sink);
    if(!classified){
        using Failure=detail::FrequencyClassifierFailure;
        const auto [code,reason]=[&]()->std::pair<std::string_view,std::string_view>{
            switch(classified.error()){
            case Failure::ordering:return {"monitoring.frequency.invalid_input","ordering"};
            case Failure::interval:return {"monitoring.frequency.invalid_input","interval"};
            case Failure::boundary_overlap:return {"monitoring.frequency.invalid_input","boundary_overlap"};
            case Failure::ratio:return {"monitoring.frequency.invalid_input","ratio"};
            case Failure::domain_order:return {"monitoring.frequency.invalid_input","domain_order"};
            case Failure::domain:return {"monitoring.frequency.invalid_input","domain"};
            case Failure::metadata:return {"monitoring.frequency.association_mismatch","metadata"};
            case Failure::implementation:return {"monitoring.frequency.association_mismatch","implementation"};
            case Failure::source:return {"monitoring.frequency.association_mismatch","source"};
            case Failure::driver:return {"monitoring.frequency.capture_incomplete","driver"};
            case Failure::sample_count:return {"monitoring.frequency.capture_incomplete","sample_count"};
            case Failure::leading_gap:return {"monitoring.frequency.capture_incomplete","leading_gap"};
            case Failure::internal_gap:return {"monitoring.frequency.capture_incomplete","internal_gap"};
            case Failure::trailing_gap:return {"monitoring.frequency.capture_incomplete","trailing_gap"};
            }
            std::terminate();
        }();
        return std::unexpected{frequency_error(code,reason)};
    }
    for(std::size_t index{};index<domains.size();++index){
        const auto& value=*results[index];
        evidence.push_back(FrequencyDomainEvidence::create(
            *value.domain,value.kind,value.state,value.minimum,value.maximum,
            value.range,value.variation,value.count));
    }
    return FrequencyObservationEvidence{
        *classified,std::move(evidence),interval,completeness};
}

} // namespace epycbench::monitoring
