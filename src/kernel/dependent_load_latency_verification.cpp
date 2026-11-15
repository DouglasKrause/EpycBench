#include "epycbench/kernel/dependent_load_latency.hpp"

#include "canonical_latency_permutation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::kernel {
namespace {

[[nodiscard]] auto verification_error(std::string_view code,
                                      std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Dependent-load latency terminal verification preparation failed",
            .operation = "kernel.latency.verification.prepare",
            .os_error = std::nullopt,
            .context = {{"phase", "terminal_verification_preparation"},
                        {"implementation", "scalar-u64-random-cycle/v1"},
                        {"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

} // namespace

PreparedDependentLoadLatencyVerification::PreparedDependentLoadLatencyVerification(
    std::vector<std::uint64_t> permutation,
    std::vector<std::size_t> inverse_position) noexcept
    : permutation_{std::move(permutation)},
      inverse_position_{std::move(inverse_position)} {}

auto PreparedDependentLoadLatencyVerification::prepare(
    const PreparedDependentLoadLatency& prepared,
    std::uint64_t resolved_permutation_seed) noexcept
    -> std::expected<PreparedDependentLoadLatencyVerification, core::Error> {
    try {
        auto permutation = detail::canonical_latency_permutation(
            prepared.node_count(), resolved_permutation_seed);
        if (!permutation) {
            return std::unexpected{verification_error(
                "kernel.latency.verification.preparation_failed",
                "permutation_stream_exhausted")};
        }

        std::vector<std::size_t> inverse_position(permutation->size());
        for (std::size_t position = 0; position < permutation->size(); ++position) {
            const auto node = static_cast<std::size_t>((*permutation)[position]);
            inverse_position[node] = position;
        }
        return PreparedDependentLoadLatencyVerification{
            std::move(*permutation), std::move(inverse_position)};
    } catch (const std::exception&) {
        return std::unexpected{verification_error(
            "kernel.latency.verification.contained_exception", "contained_exception")};
    } catch (...) {
        return std::unexpected{verification_error(
            "kernel.latency.verification.contained_unknown_failure",
            "contained_unknown_failure")};
    }
}

auto PreparedDependentLoadLatencyVerification::expected_terminal(
    const DependentLoadLatencySchedule& schedule) const noexcept
    -> DependentLoadLatencyExpectedTerminalValue {
    const auto n = permutation_.size();
    const auto position = inverse_position_[schedule.start_index()];
    const auto n64 = static_cast<std::uint64_t>(n);
    const auto offset64 = schedule.step_count().value() % n64;
    const auto offset = static_cast<std::size_t>(offset64);
    const auto distance_to_end = n - position;
    const auto target_position =
        offset >= distance_to_end ? offset - distance_to_end : position + offset;
    return DependentLoadLatencyExpectedTerminalValue{
        static_cast<std::size_t>(permutation_[target_position])};
}

DependentLoadLatencyExpectedTerminalValue::
    DependentLoadLatencyExpectedTerminalValue(std::size_t value) noexcept
    : value_{value} {}

DependentLoadLatencyVerification::DependentLoadLatencyVerification(
    DependentLoadLatencyExpectedTerminalValue expected,
    DependentLoadLatencyTerminalValue actual) noexcept
    : expected_{expected}, actual_{actual} {}

auto verify_dependent_load_latency_terminal(
    DependentLoadLatencyExpectedTerminalValue expected,
    DependentLoadLatencyTerminalValue actual) noexcept
    -> DependentLoadLatencyVerification {
    return DependentLoadLatencyVerification{expected, actual};
}

} // namespace epycbench::kernel
