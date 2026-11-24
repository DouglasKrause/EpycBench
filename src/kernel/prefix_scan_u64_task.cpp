#include "epycbench/kernel/prefix_scan_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace epycbench::kernel {
namespace {
static_assert(16U * prefix_scan_u64_maximum_count <=
              static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()));
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
        .code = core::StableErrorCode{"kernel.task.prefix_scan_u64." + std::string{reason}},
        .message = "Inclusive unsigned prefix scan validation failed", .operation = "kernel.task.prefix_scan_u64",
        .os_error = {}, .context = {{"classification", reason}}, .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment};
}
void write(std::span<std::byte> bytes, std::size_t index, std::uint64_t word) {
    for (unsigned b{}; b < 8U; ++b)
        bytes[index * 8U + b] = static_cast<std::byte>((word >> (8U * b)) & 0xffU);
}
auto read(std::span<const std::byte> bytes, std::size_t index) -> std::uint64_t {
    std::uint64_t word{};
    for (unsigned b{}; b < 8U; ++b)
        word |= std::uint64_t{std::to_integer<unsigned char>(bytes[index * 8U + b])} << (8U * b);
    return word;
}
auto generated(const PrefixScanU64Config& config, std::uint64_t index) noexcept -> std::uint64_t {
    return detail::mix64_v1(config.input_seed() + index);
}
} // namespace

auto PrefixScanU64Config::create(std::uint64_t count, std::uint64_t seed)
    -> std::expected<PrefixScanU64Config, core::Error> {
    if (!count || count > prefix_scan_u64_maximum_count)
        return std::unexpected{error("element_count_out_of_range")};
    return PrefixScanU64Config{*ElementCount::create(count), seed};
}
auto PrefixScanU64Config::work(TaskRepetitionCount count) const -> std::expected<PrefixScanU64Work, core::Error> {
    const auto work = TaskWork::create(elements_, count);
    if (!work) return std::unexpected{std::move(work.error())};
    return PrefixScanU64Work{work->completed_tasks(),
        core::WorkCount::from_base_units(work->processed_elements().value())};
}
auto initialize_prefix_scan_u64_input(const PrefixScanU64Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < config.element_count().value(); ++i) write(input, i, generated(config, i));
    return {};
}
auto prepare_prefix_scan_u64_reference(const PrefixScanU64Config& config,
    std::span<const std::byte> input, std::span<std::byte> output)
    -> std::expected<PrefixScanU64Reference, core::Error> {
    if (input.size() < config.native_bytes() || output.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    const auto count = config.element_count().value();
    std::uint64_t answer{};
    for (std::size_t i{}; i < count; ++i) {
        const auto value = generated(config, i);
        if (read(input, i) != value) return std::unexpected{error("input_content_mismatch")};
        // Each input contributes to N-i output prefixes. This reference does
        // not fold the timed scan's output or copy its loop-carried answer.
        answer += (count - i) * value;
    }
    std::uint64_t prefix{};
    for (std::size_t i{}; i < count; ++i) {
        prefix += generated(config, i);
        write(output, i, prefix);
    }
    std::ranges::fill(output.subspan(config.native_bytes()), prefix_scan_u64_padding_sentinel);
    return PrefixScanU64Reference{config, core::sha256(input.first(config.native_bytes())),
        core::sha256(output.first(config.native_bytes())), answer};
}
auto reset_prefix_scan_u64_output(const PrefixScanU64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    std::uint64_t prefix{};
    for (std::size_t i{}; i < config.element_count().value(); ++i) {
        prefix += generated(config, i);
        write(output, i, ~prefix);
    }
    std::ranges::fill(output.subspan(config.native_bytes()), prefix_scan_u64_padding_sentinel);
    return {};
}
auto verify_prefix_scan_u64_output(const PrefixScanU64Reference& reference, std::span<const std::byte> output)
    -> std::expected<PrefixScanU64Verification, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    bool content = true;
    std::uint64_t prefix{};
    for (std::size_t i{}; i < config.element_count().value(); ++i) {
        prefix += generated(config, i);
        content = (read(output, i) == prefix) && content;
    }
    const auto padding = std::ranges::all_of(output.subspan(config.native_bytes()), [](std::byte byte) {
        return byte == prefix_scan_u64_padding_sentinel;
    });
    return PrefixScanU64Verification{config.element_count(), reference.expected_output_digest(),
        core::sha256(output.first(config.native_bytes())), content, padding};
}
} // namespace epycbench::kernel
