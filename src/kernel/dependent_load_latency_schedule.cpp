#include "epycbench/kernel/dependent_load_latency.hpp"

#include "epycbench/core/seed.hpp"
#include "epycbench/kernel/kernel_id.hpp"
#include "bounded_index.hpp"
#include "mix64.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
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

[[nodiscard]] auto exhausted() -> core::Error {
    return schedule_error("kernel.latency.schedule.bounded_index_exhausted",
                          "draw_stream_exhausted");
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

DependentLoadLatencySchedule::DependentLoadLatencySchedule(
    std::size_t start_index, DependentLoadLatencyStepCount step_count) noexcept
    : start_index_{start_index}, step_count_{step_count} {}

auto DependentLoadLatencySchedule::create(
    const PreparedDependentLoadLatency& prepared, std::uint64_t root_seed,
    core::ExperimentId experiment_id, DependentLoadLatencyBlockId block_id,
    DependentLoadLatencyStepCount step_count) noexcept
    -> std::expected<DependentLoadLatencySchedule, core::Error> {
    try {
        const core::SeedDerivationKey key{
            core::SeedDomain::observation_start,
            {{"experiment", std::to_string(experiment_id.value())},
             {"block", std::to_string(block_id.value())},
             {"component", std::string{kernel_id_token(
                                KernelId::dependent_load_latency_v1)}}},
        };
        const auto derived = core::derive_seed(root_seed, key);
        if (!derived) {
            return std::unexpected{derived.error()};
        }

        const auto bound = static_cast<std::uint64_t>(prepared.node_count());
        std::uint64_t ordinal = 0U;
        for (;;) {
            if (ordinal == std::numeric_limits<std::uint64_t>::max()) {
                return std::unexpected{exhausted()};
            }
            const auto draw = detail::mix64_v1(derived->value() + ordinal);
            ++ordinal;
            const auto selected = detail::bounded_index(draw, bound);
            if (selected) {
                return DependentLoadLatencySchedule{
                    static_cast<std::size_t>(*selected), step_count};
            }
        }
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

} // namespace epycbench::kernel
