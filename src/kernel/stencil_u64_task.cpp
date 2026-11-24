#include "epycbench/kernel/stencil_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>

namespace epycbench::kernel {
namespace {
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.stencil_u64." + std::string{reason}},
            .message = "Unsigned matrix stencil validation failed", .operation = "kernel.task.stencil_u64",
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
auto grid_word(const StencilU64Config& config, std::size_t index, bool output) -> std::uint64_t {
    if (index == 0U) return stencil_u64_input_magic;
    if (index == 1U) return config.rows();
    if (index == 2U) return config.columns();
    const auto cell = index - stencil_u64_header_words;
    const auto source = [&](std::uint64_t ordinal) { return detail::mix64_v1(config.input_seed() + ordinal); };
    if (!output) return source(cell);
    const auto width = config.columns();
    const auto row = cell / width;
    const auto column = cell % width;
    auto result = source(cell);
    if (row != 0U) result += source(cell - width);
    if (column != 0U) result += source(cell - 1U);
    if (row + 1U < config.rows()) result += source(cell + width);
    if (column + 1U < width) result += source(cell + 1U);
    return result;
}

} // namespace

auto StencilU64Config::create(std::uint64_t rows, std::uint64_t columns, std::uint64_t seed)
    -> std::expected<StencilU64Config, core::Error> {
    if (rows == 0U || rows > stencil_u64_maximum_dimension || columns == 0U || columns > stencil_u64_maximum_dimension)
        return std::unexpected{error("dimension_out_of_range")};
    return StencilU64Config{rows, columns, seed};
}
auto initialize_stencil_u64_input(const StencilU64Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) write_word(input, i, grid_word(config, i, false));
    return {};
}
auto prepare_stencil_u64_reference(const StencilU64Config& config, std::span<const std::byte> input,
                                     std::span<std::byte> output) -> std::expected<StencilU64Reference, core::Error> {
    if (input.size() < config.native_bytes() || output.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    std::uint64_t answer{};
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) {
        const auto value = read_word(input, i);
        if (value != grid_word(config, i, false)) return std::unexpected{error("input_content_mismatch")};
        if (i >= stencil_u64_header_words) {
            const auto cell = i - stencil_u64_header_words;
            const auto row = cell / config.columns();
            const auto column = cell % config.columns();
            // Each source word contributes to its own output and each adjacent output.
            const auto uses = 1U + (row != 0U) + (column != 0U) +
                              (row + 1U < config.rows()) + (column + 1U < config.columns());
            answer += value * uses;
        }
    }
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) write_word(output, i, grid_word(config, i, true));
    std::ranges::fill(output.subspan(config.native_bytes()), stencil_u64_padding_sentinel);
    return StencilU64Reference{config, core::sha256(input.first(config.native_bytes())),
                                core::sha256(output.first(config.native_bytes())), answer};
}
auto reset_stencil_u64_output(const StencilU64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) write_word(output, i, ~grid_word(config, i, true));
    std::ranges::fill(output.subspan(config.native_bytes()), stencil_u64_padding_sentinel);
    return {};
}
auto verify_stencil_u64_output(const StencilU64Reference& reference, std::span<const std::byte> output)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    bool matches = true;
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i)
        matches &= read_word(output, i) == grid_word(config, i, true);
    const bool padding = std::ranges::all_of(output.subspan(config.native_bytes()),
                                              [](std::byte v) { return v == stencil_u64_padding_sentinel; });
    return XorU64Verification{config.element_count(), reference.expected_output_digest(),
                              core::sha256(output.first(config.native_bytes())), matches, padding};
}
} // namespace epycbench::kernel
