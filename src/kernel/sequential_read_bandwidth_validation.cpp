#include "epycbench/kernel/sequential_read_bandwidth.hpp"

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::kernel {
namespace {

inline constexpr std::uintptr_t required_alignment = 64U;

[[nodiscard]] auto validation_error(std::string_view code,
                                    std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Bandwidth execution input validation failed",
            .operation = "kernel.bandwidth.validation",
            .os_error = std::nullopt,
            .context = {{"phase", "execution_input_validation"},
                        {"implementation", "scalar-u64-sequential/v1"},
                        {"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto invalid_input(std::string_view classification) -> core::Error {
    return validation_error("kernel.bandwidth.validation.invalid_input", classification);
}

[[nodiscard]] auto contained_standard() -> core::Error {
    return validation_error("kernel.bandwidth.validation.contained_exception",
                            "contained_exception");
}

[[nodiscard]] auto contained_unknown() -> core::Error {
    return validation_error("kernel.bandwidth.validation.contained_unknown_failure",
                            "contained_unknown_failure");
}

} // namespace

SequentialReadBandwidthInput::SequentialReadBandwidthInput(
    SequentialReadBandwidthConfig configuration,
    std::span<const std::byte> logical_region) noexcept
    : configuration_{std::move(configuration)}, logical_region_{logical_region} {}

auto SequentialReadBandwidthInput::create(
    SequentialReadBandwidthConfig configuration,
    std::span<const std::byte> logical_region) noexcept
    -> std::expected<SequentialReadBandwidthInput, core::Error> {
    try {
        if (!std::cmp_equal(logical_region.size_bytes(),
                            configuration.working_set_per_worker().value())) {
            return std::unexpected{invalid_input("region_size_mismatch")};
        }

        const auto address = reinterpret_cast<std::uintptr_t>(logical_region.data());
        if (address % required_alignment != 0U) {
            return std::unexpected{invalid_input("region_alignment_invalid")};
        }

        return SequentialReadBandwidthInput{std::move(configuration), logical_region};
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

} // namespace epycbench::kernel
