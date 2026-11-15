#pragma once

#include "monitoring/host_qualification_composition.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {

enum class HostBaselinePreparationFailure : unsigned char {
    invalid_input, foreign_input, capacity, native_arithmetic,
    projection, digest, allocation, contained_exception
};

struct HostBaselinePreparationLimits final {
    std::size_t bandwidth_workers{8};
    core::ByteCount peak_mapping{core::ByteCount::from_base_units(UINT64_C(17179869184))};
    core::ByteCount retained_verification{core::ByteCount::from_base_units(UINT64_C(4294967296))};
    std::size_t projection_bytes{524288};
};

struct HostBaselineResourceSummary final {
    core::ByteCount bandwidth_mapping;
    core::ByteCount latency_mapping;
    core::ByteCount peak_mapping;
    core::ByteCount retained_verification;
    std::size_t bandwidth_measurement_workers;
    std::size_t latency_measurement_workers;
    std::size_t peak_measurement_workers;
    auto operator==(const HostBaselineResourceSummary&) const -> bool = default;
};

struct HostBaselineRegionDemand final {
    core::ByteCount requested{core::ByteCount::from_base_units(0)};
    core::ByteCount page{core::ByteCount::from_base_units(0)};
};

// Pure checked seams used by the factory; none establishes admission/authority.
[[nodiscard]] auto checked_host_baseline_payload_bytes(std::uint64_t, std::uint64_t) noexcept
    -> std::expected<std::uint64_t, HostBaselinePreparationFailure>;
[[nodiscard]] auto calculate_host_baseline_latency_verification(std::uint64_t) noexcept
    -> std::expected<std::uint64_t, HostBaselinePreparationFailure>;
[[nodiscard]] auto calculate_host_baseline_resources(
    std::span<const HostBaselineRegionDemand> bandwidth_first,
    std::span<const HostBaselineRegionDemand> bandwidth_second,
    std::span<const HostBaselineRegionDemand> latency_first,
    std::span<const HostBaselineRegionDemand> latency_second,
    HostBaselinePreparationLimits = {}) noexcept
    -> std::expected<HostBaselineResourceSummary, HostBaselinePreparationFailure>;

class HostBaselinePreparation final {
  public:
    HostBaselinePreparation(const HostBaselinePreparation&) = delete;
    auto operator=(const HostBaselinePreparation&) -> HostBaselinePreparation& = delete;
    HostBaselinePreparation(HostBaselinePreparation&&) noexcept;
    auto operator=(HostBaselinePreparation&&) noexcept -> HostBaselinePreparation&;
    ~HostBaselinePreparation() noexcept;
    [[nodiscard]] auto has_value() const noexcept -> bool;
    [[nodiscard]] auto physical_projection() const noexcept -> std::string_view;
    [[nodiscard]] auto physical_sha256() const noexcept -> std::optional<core::Sha256Digest>;
    [[nodiscard]] auto resources() const noexcept -> std::optional<HostBaselineResourceSummary>;

  private:
    struct State;
    explicit HostBaselinePreparation(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend auto prepare_host_baseline(const RunnerContextAcquisitionSession&,
        const MonitoringHostQualificationPlan&, const HostQualificationCalibrationInputs&,
        HostBaselinePreparationLimits) noexcept
        -> std::expected<HostBaselinePreparation, HostBaselinePreparationFailure>;
};

[[nodiscard]] auto prepare_host_baseline(const RunnerContextAcquisitionSession&,
    const MonitoringHostQualificationPlan&, const HostQualificationCalibrationInputs&,
    HostBaselinePreparationLimits = {}) noexcept
    -> std::expected<HostBaselinePreparation, HostBaselinePreparationFailure>;

// One injected new-owner allocation failure; no inherited exhaustive sweep.
void fail_next_host_baseline_owner_allocation_for_test() noexcept;

} // namespace epycbench::monitoring::qualification::detail
