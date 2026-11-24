#include "epycbench/kernel/pack_u4_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>

namespace epycbench::kernel {
namespace {
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.pack_u4." + std::string{reason}},
            .message = "Unsigned 4-bit packing validation failed", .operation = "kernel.task.pack_u4",
            .os_error = {}, .context = {{"classification", reason}}, .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
auto nibble(const PackU4Config& config, std::size_t index) noexcept -> std::byte {
    return static_cast<std::byte>(detail::mix64_v1(config.input_seed() + index) & 15U);
}
auto packed(const PackU4Config& config, std::size_t output_index) noexcept -> std::byte {
    const auto i = 2U * output_index;
    auto byte = nibble(config, i);
    if (i + 1U < config.native_bytes()) byte |= nibble(config, i + 1U) << 4;
    return byte;
}
} // namespace

auto PackU4Config::create(std::uint64_t elements, std::uint64_t seed) -> std::expected<PackU4Config, core::Error> {
    if (elements == 0U || elements > pack_u4_maximum_count) return std::unexpected{error("element_count_out_of_range")};
    return PackU4Config{*ElementCount::create(elements), seed};
}
auto PackU4Config::work(TaskRepetitionCount repetitions) const -> std::expected<PackU4Work, core::Error> {
    auto task = TaskWork::create(elements_, repetitions);
    if (!task) return std::unexpected{std::move(task.error())};
    // Each packed byte represents at least one input element; the checked input work bounds both products.
    return PackU4Work{repetitions, core::WorkCount::from_base_units(task->processed_elements().value()),
                      core::ByteCount::from_base_units(repetitions.value() * output_bytes().value())};
}
auto initialize_pack_u4_input(const PackU4Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes()) return std::unexpected{error("input_too_small")};
    for (std::size_t i{}; i < config.native_bytes(); ++i) input[i] = nibble(config, i);
    return {};
}
auto prepare_pack_u4_reference(const PackU4Config& config, std::span<const std::byte> input,
                               std::span<std::byte> output) -> std::expected<PackU4Reference, core::Error> {
    const auto bytes = static_cast<std::size_t>(config.output_bytes().value());
    if (input.size() < config.native_bytes() || output.size() < bytes)
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    for (std::size_t i{}; i < config.native_bytes(); ++i)
        if (input[i] != nibble(config, i)) return std::unexpected{error("canonical_input_mismatch")};
    std::uint64_t answer{};
    for (std::size_t i{}; i < bytes; ++i) {
        output[i] = packed(config, i);
        answer += std::to_integer<unsigned>(output[i]);
    }
    std::ranges::fill(output.subspan(bytes), pack_u4_padding_sentinel);
    return PackU4Reference{config, core::sha256(input.first(config.native_bytes())), core::sha256(output.first(bytes)), answer};
}
auto reset_pack_u4_output(const PackU4Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& config = reference.configuration();
    const auto bytes = static_cast<std::size_t>(config.output_bytes().value());
    if (output.size() < bytes) return std::unexpected{error("output_too_small")};
    for (std::size_t i{}; i < bytes; ++i) output[i] = ~packed(config, i);
    std::ranges::fill(output.subspan(bytes), pack_u4_padding_sentinel);
    return {};
}
auto verify_pack_u4_output(const PackU4Reference& reference, std::span<const std::byte> output)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& config = reference.configuration();
    const auto bytes = static_cast<std::size_t>(config.output_bytes().value());
    if (output.size() < bytes) return std::unexpected{error("output_too_small")};
    const auto digest = core::sha256(output.first(bytes));
    const bool padding = std::ranges::all_of(output.subspan(bytes), [](std::byte v) { return v == pack_u4_padding_sentinel; });
    return XorU64Verification{config.element_count(), reference.expected_output_digest(), digest,
                              digest == reference.expected_output_digest(), padding};
}
} // namespace epycbench::kernel
