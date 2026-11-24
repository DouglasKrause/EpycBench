#include "epycbench/kernel/transpose_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>

namespace epycbench::kernel {
namespace {
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.transpose_u64." + std::string{reason}},
            .message = "Unsigned matrix transpose validation failed", .operation = "kernel.task.transpose_u64",
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
auto matrix_word(const TransposeU64Config& config, std::size_t index, bool transposed) -> std::uint64_t {
    if (index == 0U) return transpose_u64_input_magic;
    if (index == 1U) return transposed ? config.columns() : config.rows();
    if (index == 2U) return transposed ? config.rows() : config.columns();
    const auto payload = index - transpose_u64_header_words;
    // Inverse map: walk the output in row-major order, then generate its source word.
    const auto source = transposed ? (payload % config.rows()) * config.columns() + payload / config.rows() : payload;
    return detail::mix64_v1(config.input_seed() + source);
}
} // namespace

auto TransposeU64Config::create(std::uint64_t rows, std::uint64_t columns, std::uint64_t seed)
    -> std::expected<TransposeU64Config, core::Error> {
    if (rows == 0U || rows > transpose_u64_maximum_dimension || columns == 0U || columns > transpose_u64_maximum_dimension)
        return std::unexpected{error("dimension_out_of_range")};
    return TransposeU64Config{rows, columns, seed};
}
auto initialize_transpose_u64_input(const TransposeU64Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) write_word(input, i, matrix_word(config, i, false));
    return {};
}
auto prepare_transpose_u64_reference(const TransposeU64Config& config, std::span<const std::byte> input,
                                     std::span<std::byte> output) -> std::expected<TransposeU64Reference, core::Error> {
    if (input.size() < config.native_bytes() || output.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    std::uint64_t answer{};
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) {
        const auto value = read_word(input, i);
        if (value != matrix_word(config, i, false)) return std::unexpected{error("input_content_mismatch")};
        if (i >= transpose_u64_header_words) answer += value;
    }
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) write_word(output, i, matrix_word(config, i, true));
    std::ranges::fill(output.subspan(config.native_bytes()), transpose_u64_padding_sentinel);
    return TransposeU64Reference{config, core::sha256(input.first(config.native_bytes())),
                                core::sha256(output.first(config.native_bytes())), answer};
}
auto reset_transpose_u64_output(const TransposeU64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i) write_word(output, i, ~matrix_word(config, i, true));
    std::ranges::fill(output.subspan(config.native_bytes()), transpose_u64_padding_sentinel);
    return {};
}
auto verify_transpose_u64_output(const TransposeU64Reference& reference, std::span<const std::byte> output)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("storage_too_small")};
    bool matches = true;
    for (std::size_t i{}; i < config.native_bytes() / 8U; ++i)
        matches &= read_word(output, i) == matrix_word(config, i, true);
    const bool padding = std::ranges::all_of(output.subspan(config.native_bytes()),
                                              [](std::byte v) { return v == transpose_u64_padding_sentinel; });
    return XorU64Verification{config.element_count(), reference.expected_output_digest(),
                              core::sha256(output.first(config.native_bytes())), matches, padding};
}
} // namespace epycbench::kernel
