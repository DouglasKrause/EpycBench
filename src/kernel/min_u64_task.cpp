#include "epycbench/kernel/min_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <limits>

namespace epycbench::kernel {
namespace {
auto error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
        .code = core::StableErrorCode{"kernel.task.min_u64." + std::string{classification}},
        .message = "Unsigned minimum task validation failed", .operation = "kernel.task.min_u64",
        .os_error = {}, .context = {{"classification", classification}},
        .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_experiment};
}
} // namespace

auto MinU64Config::create(std::uint64_t count, std::uint64_t seed)
    -> std::expected<MinU64Config, core::Error> {
    auto input = SumU64Config::create(count, seed);
    if (!input) return std::unexpected{error("element_count_out_of_range")};
    return MinU64Config{*input};
}
auto initialize_min_u64_input(const MinU64Config& configuration, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    return initialize_sum_u64_input(configuration.input_configuration(), input);
}
auto prepare_min_u64_reference(const MinU64Config& configuration, std::span<const std::byte> input)
    -> std::expected<MinU64Reference, core::Error> {
    if (input.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    auto minimum = std::numeric_limits<std::uint64_t>::max();
    for (std::uint64_t index{}; index < configuration.element_count().value(); ++index) {
        std::uint64_t word{};
        for (unsigned byte{}; byte < 8U; ++byte)
            word |= std::uint64_t{std::to_integer<unsigned char>(input[static_cast<std::size_t>(index * 8U) + byte])}
                    << (byte * 8U);
        if (word != detail::mix64_v1(configuration.input_seed() + index))
            return std::unexpected{error("input_content_mismatch")};
        minimum = std::min(minimum, word);
    }
    return MinU64Reference{configuration, core::sha256(input.first(configuration.native_bytes())), minimum};
}
} // namespace epycbench::kernel
