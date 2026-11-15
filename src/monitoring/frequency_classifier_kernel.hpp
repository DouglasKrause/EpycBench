#pragma once

#include "epycbench/monitoring/frequency.hpp"

#include <expected>
#include <span>

namespace epycbench::monitoring::detail {

enum class FrequencyClassifierFailure : unsigned char {
    ordering,
    interval,
    boundary_overlap,
    ratio,
    domain_order,
    domain,
    metadata,
    implementation,
    source,
    driver,
    sample_count,
    leading_gap,
    internal_gap,
    trailing_gap,
};

struct FrequencyDomainClassification final {
    const FrequencyDomainId* domain{};
    FrequencySourceKind kind{};
    MonitoringSignalState state{};
    std::optional<FrequencyHz> minimum;
    std::optional<FrequencyHz> maximum;
    std::optional<FrequencyRangeHz> range;
    std::optional<FrequencyVariationRatio> variation;
    std::size_t count{};
};

struct FrequencyEvidenceSink final {
    void* context{};
    bool (*append)(void*, const FrequencyDomainClassification&) noexcept{};
};

[[nodiscard]] auto classify_frequency_bounded(
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end,
    core::Duration sampling_interval,
    FrequencyCaptureCompleteness completeness,
    std::span<const FrequencyDomainAcquisitions> domains,
    std::span<const FrequencyAcquisition*> available_scratch,
    std::span<std::optional<FrequencyDomainClassification>> result_scratch,
    FrequencyEvidenceSink sink) noexcept
    -> std::expected<MonitoringSignalState, FrequencyClassifierFailure>;

} // namespace epycbench::monitoring::detail
