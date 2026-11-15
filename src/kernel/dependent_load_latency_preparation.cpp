#include "epycbench/kernel/dependent_load_latency.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::kernel {
namespace {

[[nodiscard]] auto schedule_error(std::string_view code,
                                  std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Dependent-load latency start-state scheduling failed",
            .operation = "kernel.latency.schedule",
            .os_error = std::nullopt,
            .context = {{"phase", "start_state_schedule"},
                        {"implementation", "scalar-u64-random-cycle/v1"},
                        {"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto contained_standard() -> core::Error {
    return schedule_error("kernel.latency.schedule.contained_exception",
                          "contained_exception");
}

[[nodiscard]] auto contained_unknown() -> core::Error {
    return schedule_error("kernel.latency.schedule.contained_unknown_failure",
                          "contained_unknown_failure");
}

} // namespace

PreparedDependentLoadLatency::PreparedDependentLoadLatency(
    DependentLoadLatencyInput input, std::size_t node_count) noexcept
    : input_{std::move(input)}, node_count_{node_count} {}

auto PreparedDependentLoadLatency::prepare(DependentLoadLatencyInput input) noexcept
    -> PreparedDependentLoadLatency {
    const auto node_count = input.logical_region().size_bytes() / 64U;
    return PreparedDependentLoadLatency{std::move(input), node_count};
}

DependentLoadLatencyStepCount::DependentLoadLatencyStepCount(std::uint64_t value) noexcept
    : value_{value} {}

auto DependentLoadLatencyStepCount::create(std::uint64_t value) noexcept
    -> std::expected<DependentLoadLatencyStepCount, core::Error> {
    try {
        if (value == 0U) {
            return std::unexpected{schedule_error(
                "kernel.latency.schedule.invalid_step_count", "zero_step_count")};
        }
        return DependentLoadLatencyStepCount{value};
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

} // namespace epycbench::kernel
