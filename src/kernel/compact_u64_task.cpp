#include "epycbench/kernel/compact_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>

namespace epycbench::kernel {
namespace {
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel, .code = core::StableErrorCode{"kernel.task.compact_u64." + std::string{reason}},
        .message = "Stable stream compaction validation failed", .operation = "kernel.task.compact_u64",
        .os_error = {}, .context = {{"classification", reason}}, .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment};
}
auto word(std::span<const std::byte> bytes, std::size_t index) noexcept -> std::uint64_t {
    std::uint64_t value;
    std::memcpy(&value, bytes.data() + 8U * index, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    return value;
}
void write(std::span<std::byte> bytes, std::size_t index, std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    std::memcpy(bytes.data() + 8U * index, &value, sizeof(value));
}
template<class Visit> auto selected_words(const CompactU64Config& config, Visit visit) -> std::size_t {
    std::size_t selected{};
    for (std::size_t i{}; i < config.element_count().value(); ++i) {
        const auto value = detail::mix64_v1(config.input_seed() + i);
        if ((value & 1U) == 0U) visit(++selected, value);
    }
    return selected;
}
void fill_unused(const CompactU64Config& config, std::size_t selected, std::span<std::byte> output) {
    std::ranges::fill(output.first(config.native_bytes()).subspan(8U * (selected + 1U)), compact_u64_unused_byte);
    std::ranges::fill(output.subspan(config.native_bytes()), xor_u64_padding_sentinel);
}
} // namespace

auto CompactU64Config::create(std::uint64_t count, std::uint64_t seed) -> std::expected<CompactU64Config, core::Error> {
    if (!count || count > compact_u64_maximum_count) return std::unexpected{error("element_count_out_of_range")};
    return CompactU64Config{*ElementCount::create(count), seed};
}
auto initialize_compact_u64_input(const CompactU64Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    write(input, 0U, config.element_count().value());
    for (std::size_t i{}; i < config.element_count().value(); ++i)
        write(input, i + 1U, detail::mix64_v1(config.input_seed() + i));
    return {};
}
auto prepare_compact_u64_reference(const CompactU64Config& config, std::span<const std::byte> input,
                                   std::span<std::byte> output) -> std::expected<CompactU64Reference, core::Error> {
    if (input.size() < config.native_bytes() || output.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    if (word(input, 0U) != config.element_count().value()) return std::unexpected{error("input_header_mismatch")};
    std::uint64_t answer{};
    for (std::size_t i{}; i < config.element_count().value(); ++i) {
        const auto value = word(input, i + 1U);
        if (value != detail::mix64_v1(config.input_seed() + i)) return std::unexpected{error("input_content_mismatch")};
        if ((value & 1U) == 0U) answer += value + 1U;
    }
    const auto selected = selected_words(config, [&](std::size_t slot, std::uint64_t value) { write(output, slot, value); });
    write(output, 0U, selected);
    fill_unused(config, selected, output);
    return CompactU64Reference{config, core::sha256(input.first(config.native_bytes())),
        core::sha256(output.first(config.native_bytes())), selected, answer};
}
auto reset_compact_u64_output(const CompactU64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    const auto selected = selected_words(config, [&](std::size_t slot, std::uint64_t value) { write(output, slot, ~value); });
    write(output, 0U, ~reference.selected_elements());
    fill_unused(config, selected, output);
    return {};
}
auto verify_compact_u64_output(const CompactU64Reference& reference, std::span<const std::byte> output)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    bool matches = word(output, 0U) == reference.selected_elements();
    const auto selected = selected_words(config, [&](std::size_t slot, std::uint64_t expected) {
        matches = (word(output, slot) == expected) && matches;
    });
    const auto logical = output.first(config.native_bytes());
    const auto unused = logical.subspan(8U * (selected + 1U));
    const bool tail = std::ranges::all_of(unused, [](std::byte b) { return b == compact_u64_unused_byte; });
    const bool padding = std::ranges::all_of(output.subspan(logical.size()), [](std::byte b) { return b == xor_u64_padding_sentinel; });
    return XorU64Verification{config.element_count(), reference.expected_output_digest(), core::sha256(logical),
                              matches && tail, padding};
}
} // namespace epycbench::kernel
