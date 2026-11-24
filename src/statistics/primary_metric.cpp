#include "epycbench/statistics/primary_metric.hpp"

#include <cmath>
#include <string>

namespace epycbench::statistics {
namespace {

auto error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::statistics,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Primary metric construction failed",
            .operation = "statistics.primary_metric.create",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

constexpr double nanoseconds_per_second = 1'000'000'000.0;

} // namespace

auto Bandwidth::create(core::ByteCount bytes, core::Duration elapsed) noexcept
    -> std::expected<Bandwidth, core::Error> {
    if (bytes.value() == 0 || elapsed.value() == 0) {
        return std::unexpected{
            error("statistics.primary_metric.invalid_input", "zero_bytes_or_duration")};
    }
    const double value = static_cast<double>(bytes.value()) * nanoseconds_per_second /
                         static_cast<double>(elapsed.value());
    if (!std::isfinite(value) || value <= 0.0) {
        return std::unexpected{
            error("statistics.primary_metric.unrepresentable", "nonfinite_or_nonpositive")};
    }
    return Bandwidth{value};
}

auto TaskThroughput::create(core::WorkCount completed_tasks, core::Duration elapsed) noexcept
    -> std::expected<TaskThroughput, core::Error> {
    if (completed_tasks.value() == 0U || elapsed.value() == 0U) {
        return std::unexpected{
            error("statistics.primary_metric.invalid_input", "zero_tasks_or_duration")};
    }
    const double value = static_cast<double>(completed_tasks.value()) * nanoseconds_per_second /
                         static_cast<double>(elapsed.value());
    if (!std::isfinite(value) || value <= 0.0) {
        return std::unexpected{
            error("statistics.primary_metric.unrepresentable", "nonfinite_or_nonpositive")};
    }
    return TaskThroughput{value};
}

auto DependentLoadLatency::create(core::Duration elapsed,
                                  core::WorkCount completed_work) noexcept
    -> std::expected<DependentLoadLatency, core::Error> {
    if (elapsed.value() == 0 || completed_work.value() == 0) {
        return std::unexpected{
            error("statistics.primary_metric.invalid_input", "zero_duration_or_work")};
    }
    const double value = static_cast<double>(elapsed.value()) /
                         static_cast<double>(completed_work.value()) /
                         nanoseconds_per_second;
    if (!std::isfinite(value) || value <= 0.0) {
        return std::unexpected{
            error("statistics.primary_metric.unrepresentable", "nonfinite_or_nonpositive")};
    }
    return DependentLoadLatency{value};
}

} // namespace epycbench::statistics
