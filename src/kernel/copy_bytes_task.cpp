#include "epycbench/kernel/copy_bytes_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace epycbench::kernel {
namespace {

constexpr auto maximum_bytes = std::min<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(),
    std::numeric_limits<std::size_t>::max()) / 2U;

auto error(std::string_view classification, std::string_view parameter = {}) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    if (!parameter.empty()) context["parameter"] = parameter;
    return {.category = core::ErrorCategory::kernel,
        .code = core::StableErrorCode{"kernel.task.copy_bytes." + std::string{classification}},
        .message = "Exact byte copy task validation failed", .operation = "kernel.task.copy_bytes",
        .os_error = {}, .context = std::move(context), .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment};
}

auto generated_byte(std::uint64_t seed, std::uint64_t index) noexcept -> std::byte {
    return static_cast<std::byte>(detail::mix64_v1(seed + index) & 0xffU);
}

auto matches_payload(const CopyBytesConfig& configuration, std::span<const std::byte> logical) noexcept -> bool {
    for (std::size_t index{}; index < logical.size(); ++index)
        if (logical[index] != generated_byte(configuration.input_seed(), index)) return false;
    return true;
}

} // namespace

auto CopyBytesConfig::create(std::uint64_t count, std::uint64_t seed)
    -> std::expected<CopyBytesConfig, core::Error> {
    if (count == 0U || count > maximum_bytes)
        return std::unexpected{error("byte_count_out_of_range", "byte_count")};
    auto bytes = ElementCount::create(count);
    if (!bytes) return std::unexpected{std::move(bytes.error())};
    return CopyBytesConfig{*bytes, seed};
}

auto CopyBytesConfig::logical_bytes() const noexcept -> core::ByteCount {
    return core::ByteCount::from_base_units(bytes_.value());
}

auto CopyBytesConfig::native_bytes() const noexcept -> std::size_t {
    return static_cast<std::size_t>(bytes_.value());
}

auto CopyBytesConfig::maximum_repetitions() const noexcept -> std::uint64_t {
    return std::numeric_limits<std::uint64_t>::max() / bytes_.value();
}

auto CopyBytesConfig::work(TaskRepetitionCount repetitions) const -> std::expected<CopyBytesWork, core::Error> {
    auto work = TaskWork::create(bytes_, repetitions);
    if (!work) return std::unexpected{std::move(work.error())};
    return CopyBytesWork{work->completed_tasks(), core::ByteCount::from_base_units(work->processed_elements().value())};
}



auto initialize_copy_bytes_input(const CopyBytesConfig& configuration, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::size_t index{}; index < configuration.native_bytes(); ++index)
        input[index] = generated_byte(configuration.input_seed(), index);
    return {};
}

auto prepare_copy_bytes_reference(const CopyBytesConfig& configuration,
    std::span<const std::byte> input, std::span<std::byte> output)
    -> std::expected<CopyBytesReference, core::Error> {
    const auto count = configuration.native_bytes();
    if (input.size() < count || output.size() < count) return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    const auto logical = input.first(count);
    if (!matches_payload(configuration, logical)) return std::unexpected{error("input_content_mismatch")};
    const auto last = generated_byte(configuration.input_seed(), count - 1U);
    return CopyBytesReference{configuration, core::sha256(logical), std::to_integer<std::uint64_t>(last)};
}

auto reset_copy_bytes_output(const CopyBytesReference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& configuration = reference.configuration();
    const auto count = configuration.native_bytes();
    if (output.size() < count) return std::unexpected{error("storage_too_small")};
    for (std::size_t index{}; index < count; ++index)
        output[index] = ~generated_byte(configuration.input_seed(), index);
    std::ranges::fill(output.subspan(count), copy_bytes_padding_sentinel);
    return {};
}

auto verify_copy_bytes_output(const CopyBytesReference& reference, std::span<const std::byte> output)
    -> std::expected<CopyBytesVerification, core::Error> {
    const auto& configuration = reference.configuration();
    const auto count = configuration.native_bytes();
    if (output.size() < count) return std::unexpected{error("storage_too_small")};
    const auto logical = output.first(count);
    const auto intact = std::ranges::all_of(output.subspan(count), [](std::byte byte) {
        return byte == copy_bytes_padding_sentinel;
    });
    return CopyBytesVerification{configuration.element_count(), reference.expected_output_digest(),
        core::sha256(logical), matches_payload(configuration, logical), intact};
}

} // namespace epycbench::kernel
