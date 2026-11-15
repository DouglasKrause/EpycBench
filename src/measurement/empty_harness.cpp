#include "epycbench/measurement/empty_harness.hpp"

#include <utility>

namespace epycbench::measurement {
namespace {

[[nodiscard]] auto production_now(void*) noexcept
    -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
    return platform_linux::raw_monotonic_now();
}

} // namespace

auto detail::production_raw_monotonic_clock_operations() noexcept
    -> RawMonotonicClockOperations {
    return {.context = nullptr, .now = production_now};
}

EmptyHarnessTrial::EmptyHarnessTrial(MeasurementInterval interval) noexcept
    : interval_{std::move(interval)} {}

auto EmptyHarnessTrial::create(MeasurementInterval interval) noexcept -> EmptyHarnessTrial {
    return EmptyHarnessTrial{std::move(interval)};
}

auto EmptyHarnessTrial::interval() const noexcept -> const MeasurementInterval& {
    return interval_;
}

auto detail::EmptyHarnessAccess::run(const RawMonotonicClockOperations& operations)
    -> std::expected<EmptyHarnessTrial, core::Error> {
    auto start = operations.now(operations.context);
    if (!start) {
        return std::unexpected{std::move(start.error())};
    }

    auto end = operations.now(operations.context);
    if (!end) {
        return std::unexpected{std::move(end.error())};
    }

    auto interval = MeasurementInterval::create(*start, *end);
    if (!interval) {
        return std::unexpected{std::move(interval.error())};
    }
    return EmptyHarnessTrial::create(std::move(*interval));
}

auto run_empty_harness_trial() -> std::expected<EmptyHarnessTrial, core::Error> {
    return detail::EmptyHarnessAccess::run(
        detail::production_raw_monotonic_clock_operations());
}

} // namespace epycbench::measurement
