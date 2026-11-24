#include "epycbench/kernel/scatter_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>

namespace epycbench::kernel {
namespace {
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.scatter_u64." + std::string{reason}},
            .message = "Keyed record scatter validation failed", .operation = "kernel.task.scatter_u64",
            .os_error = {}, .context = {{"classification", reason}}, .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
auto read_word(std::span<const std::byte> bytes, std::size_t index) -> std::uint64_t {
    std::uint64_t value{};
    for (unsigned i{}; i < 8U; ++i)
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[8U * index + i])} << (8U * i);
    return value;
}
void write_word(std::span<std::byte> bytes, std::size_t index, std::uint64_t value) {
    for (unsigned i{}; i < 8U; ++i)
        bytes[8U * index + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
}
auto ordered_word(const ScatterU64Config& config, std::size_t index) -> std::uint64_t {
    if (index == 0U) return scatter_u64_input_magic;
    if (index == 1U) return config.element_count().value();
    if (index == 2U) return config.input_seed();
    const auto key = static_cast<std::uint64_t>(index - scatter_u64_header_words);
    return (key << 48U) | (detail::mix64_v1(config.input_seed() + key) & UINT64_C(0x0000ffffffffffff));
}
} // namespace

auto ScatterU64Config::create(std::uint64_t count, std::uint64_t seed) -> std::expected<ScatterU64Config, core::Error> {
    if (count == 0U || count > scatter_u64_maximum_count) return std::unexpected{error("count_out_of_range")};
    return ScatterU64Config{count, seed};
}
auto initialize_scatter_u64_input(const ScatterU64Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    const auto count = static_cast<std::size_t>(config.element_count().value());
    for (std::size_t i{}; i < scatter_u64_header_words + count; ++i)
        write_word(input, i, ordered_word(config, i));
    for (auto remaining = count; remaining > 1U; --remaining) {
        const auto draw = detail::mix64_v1((config.input_seed() ^ UINT64_C(0xd1b54a32d192ed03)) + remaining);
        const auto first = scatter_u64_header_words + remaining - 1U;
        const auto second = scatter_u64_header_words + static_cast<std::size_t>(draw % remaining);
        const auto word = read_word(input, first);
        write_word(input, first, read_word(input, second));
        write_word(input, second, word);
    }
    return {};
}
auto prepare_scatter_u64_reference(const ScatterU64Config& config, std::span<const std::byte> input,
                                   std::span<std::byte> output) -> std::expected<ScatterU64Reference, core::Error> {
    if (input.size() < config.native_bytes() || output.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    // The output owner also provides disposable storage for checking the exact input permutation.
    if (auto generated = initialize_scatter_u64_input(config, output); !generated) return std::unexpected{generated.error()};
    if (!std::ranges::equal(input.first(config.native_bytes()), output.first(config.native_bytes())))
        return std::unexpected{error("input_content_mismatch")};
    std::uint64_t answer{};
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) {
        const auto word = ordered_word(config, i);
        write_word(output, i, word);
        if (i >= scatter_u64_header_words) answer += word;
    }
    std::ranges::fill(output.subspan(config.native_bytes()), scatter_u64_padding_sentinel);
    return ScatterU64Reference{config, core::sha256(input.first(config.native_bytes())),
                               core::sha256(output.first(config.native_bytes())), answer};
}
auto reset_scatter_u64_output(const ScatterU64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) write_word(output, i, ~ordered_word(config, i));
    std::ranges::fill(output.subspan(config.native_bytes()), scatter_u64_padding_sentinel);
    return {};
}
auto verify_scatter_u64_output(const ScatterU64Reference& reference, std::span<const std::byte> output)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    bool matches = true;
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i)
        matches &= read_word(output, i) == ordered_word(config, i);
    const bool padding = std::ranges::all_of(output.subspan(config.native_bytes()),
                                              [](std::byte v) { return v == scatter_u64_padding_sentinel; });
    return XorU64Verification{config.element_count(), reference.expected_output_digest(),
                              core::sha256(output.first(config.native_bytes())), matches, padding};
}
} // namespace epycbench::kernel
