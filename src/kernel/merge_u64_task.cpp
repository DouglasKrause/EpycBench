#include "epycbench/kernel/merge_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace epycbench::kernel {
namespace {
static_assert(16U * merge_u64_maximum_count <=
              static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()));
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
        .code = core::StableErrorCode{"kernel.task.merge_u64." + std::string{reason}},
        .message = "Unsigned sorted-run merge validation failed", .operation = "kernel.task.merge_u64",
        .os_error = {}, .context = {{"classification", reason}}, .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment};
}
void write(std::span<std::byte> bytes, std::size_t index, std::uint64_t word) noexcept {
    for (unsigned b{}; b < 8U; ++b)
        bytes[index * 8U + b] = static_cast<std::byte>((word >> (8U * b)) & 0xffU);
}
auto read(std::span<const std::byte> bytes, std::size_t index) noexcept -> std::uint64_t {
    std::uint64_t word{};
    for (unsigned b{}; b < 8U; ++b)
        word |= std::uint64_t{std::to_integer<unsigned char>(bytes[index * 8U + b])} << (8U * b);
    return word;
}
auto sorted_word(const MergeU64Config& config, std::uint64_t index) noexcept -> std::uint64_t {
    const auto bucket = index / 2U;
    const auto width = UINT64_MAX / ((config.element_count().value() + 1U) / 2U);
    return bucket * width + detail::mix64_v1(config.input_seed() + bucket) % width;
}
auto input_word(const MergeU64Config& config, std::size_t index) noexcept -> std::uint64_t {
    if (config.input_pattern() == MergeU64InputPattern::separated) return sorted_word(config, index);
    return sorted_word(config, index < config.midpoint() ? 2U * index + 1U
                                                       : 2U * (index - config.midpoint()));
}
} // namespace

auto MergeU64Config::create(std::uint64_t count, MergeU64InputPattern pattern, std::uint64_t seed)
    -> std::expected<MergeU64Config, core::Error> {
    if (count < 2U || count > merge_u64_maximum_count)
        return std::unexpected{error("element_count_out_of_range")};
    if (merge_u64_input_pattern_token(pattern).empty()) return std::unexpected{error("invalid_input_pattern")};
    return MergeU64Config{*ElementCount::create(count), pattern, seed};
}
auto MergeU64Config::work(TaskRepetitionCount count) const -> std::expected<MergeU64Work, core::Error> {
    const auto work = TaskWork::create(elements_, count);
    if (!work) return std::unexpected{std::move(work.error())};
    return MergeU64Work{work->completed_tasks(), core::WorkCount::from_base_units(work->processed_elements().value())};
}
auto initialize_merge_u64_input(const MergeU64Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < config.element_count().value(); ++i) write(input, i, input_word(config, i));
    return {};
}
auto prepare_merge_u64_reference(const MergeU64Config& config,
    std::span<const std::byte> input, std::span<std::byte> output)
    -> std::expected<MergeU64Reference, core::Error> {
    if (input.size() < config.native_bytes() || output.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    const auto count = config.element_count().value();
    // Check both entire canonical runs before writing any expected-output byte.
    for (std::size_t i{}; i < count; ++i)
        if (read(input, i) != input_word(config, i)) return std::unexpected{error("input_content_mismatch")};
    std::uint64_t answer{};
    for (std::size_t i{}; i < count; ++i) {
        const auto word = sorted_word(config, i);
        answer += (i + 1U) * word;
        write(output, i, word);
    }
    std::ranges::fill(output.subspan(config.native_bytes()), merge_u64_padding_sentinel);
    return MergeU64Reference{config, core::sha256(input.first(config.native_bytes())),
        core::sha256(output.first(config.native_bytes())), answer};
}
auto reset_merge_u64_output(const MergeU64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < config.element_count().value(); ++i) write(output, i, ~sorted_word(config, i));
    std::ranges::fill(output.subspan(config.native_bytes()), merge_u64_padding_sentinel);
    return {};
}
auto verify_merge_u64_output(const MergeU64Reference& reference, std::span<const std::byte> output)
    -> std::expected<MergeU64Verification, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    bool content = true;
    for (std::size_t i{}; i < config.element_count().value(); ++i)
        content = (read(output, i) == sorted_word(config, i)) && content;
    const auto padding = std::ranges::all_of(output.subspan(config.native_bytes()), [](std::byte byte) {
        return byte == merge_u64_padding_sentinel;
    });
    return MergeU64Verification{config.element_count(), reference.expected_output_digest(),
        core::sha256(output.first(config.native_bytes())), content, padding};
}
} // namespace epycbench::kernel
