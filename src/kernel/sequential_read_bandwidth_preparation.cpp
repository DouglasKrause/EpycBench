#include "epycbench/kernel/sequential_read_bandwidth.hpp"

#include <bit>
#include <climits>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::kernel {
namespace {

static_assert(CHAR_BIT == 8);
static_assert(sizeof(std::uint64_t) == 8U);
static_assert(std::endian::native == std::endian::little);

[[nodiscard]] auto pass_count_error(std::string_view code,
                                    std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Bandwidth pass count validation failed",
            .operation = "kernel.bandwidth.pass_count",
            .os_error = std::nullopt,
            .context = {{"phase", "pass_count_validation"},
                        {"implementation", "scalar-u64-sequential/v1"},
                        {"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto invalid_pass_count() -> core::Error {
    return pass_count_error("kernel.bandwidth.pass_count.invalid", "zero_pass_count");
}

[[nodiscard]] auto contained_standard() -> core::Error {
    return pass_count_error("kernel.bandwidth.pass_count.contained_exception",
                            "contained_exception");
}

[[nodiscard]] auto contained_unknown() -> core::Error {
    return pass_count_error("kernel.bandwidth.pass_count.contained_unknown_failure",
                            "contained_unknown_failure");
}

} // namespace

PreparedSequentialReadBandwidth::PreparedSequentialReadBandwidth(
    SequentialReadBandwidthInput input, std::size_t word_count) noexcept
    : input_{std::move(input)}, word_count_{word_count} {}

auto PreparedSequentialReadBandwidth::prepare(SequentialReadBandwidthInput input) noexcept
    -> PreparedSequentialReadBandwidth {
    const auto word_count = input.logical_region().size_bytes() / 8U;
    return PreparedSequentialReadBandwidth{std::move(input), word_count};
}

SequentialReadBandwidthPassCount::SequentialReadBandwidthPassCount(std::uint64_t value) noexcept
    : value_{value} {}

auto SequentialReadBandwidthPassCount::create(std::uint64_t value) noexcept
    -> std::expected<SequentialReadBandwidthPassCount, core::Error> {
    try {
        if (value == 0U) {
            return std::unexpected{invalid_pass_count()};
        }
        return SequentialReadBandwidthPassCount{value};
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

} // namespace epycbench::kernel
