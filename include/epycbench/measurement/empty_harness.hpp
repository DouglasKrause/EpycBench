#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/measurement/observation.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"

#include <expected>

namespace epycbench::measurement {

class EmptyHarnessTrial {
  public:
    EmptyHarnessTrial(const EmptyHarnessTrial&) noexcept = default;
    EmptyHarnessTrial(EmptyHarnessTrial&&) noexcept = default;
    auto operator=(const EmptyHarnessTrial&) -> EmptyHarnessTrial& = delete;
    auto operator=(EmptyHarnessTrial&&) -> EmptyHarnessTrial& = delete;
    ~EmptyHarnessTrial() noexcept = default;

    [[nodiscard]] static auto create(MeasurementInterval interval) noexcept
        -> EmptyHarnessTrial;

    [[nodiscard]] auto interval() const noexcept -> const MeasurementInterval&;

  private:
    explicit EmptyHarnessTrial(MeasurementInterval interval) noexcept;

    const MeasurementInterval interval_;
};

[[nodiscard]] auto run_empty_harness_trial()
    -> std::expected<EmptyHarnessTrial, core::Error>;

namespace detail {

struct RawMonotonicClockOperations {
    void* context;
    std::expected<platform_linux::RawMonotonicTimestamp, core::Error> (*now)(void*) noexcept;
};

[[nodiscard]] auto production_raw_monotonic_clock_operations() noexcept
    -> RawMonotonicClockOperations;

class EmptyHarnessAccess {
  public:
    [[nodiscard]] static auto run(const RawMonotonicClockOperations& operations)
        -> std::expected<EmptyHarnessTrial, core::Error>;
};

} // namespace detail

} // namespace epycbench::measurement
