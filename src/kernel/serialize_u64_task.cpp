#include "epycbench/kernel/serialize_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>

namespace epycbench::kernel {
namespace {
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.serialize_u64." + std::string{reason}},
            .message = "Network-order serialization validation failed", .operation = "kernel.task.serialize_u64",
            .os_error = {}, .context = {{"classification", reason}}, .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
} // namespace

auto SerializeU64Config::create(std::uint64_t records, std::uint64_t seed)
    -> std::expected<SerializeU64Config, core::Error> {
    if (records == 0U || records > serialize_u64_maximum_count)
        return std::unexpected{error("record_count_out_of_range")};
    auto input = SumU64Config::create(records, seed);
    if (!input) return std::unexpected{std::move(input.error())};
    return SerializeU64Config{*input};
}

auto SerializeU64Config::work(TaskRepetitionCount repetitions) const -> std::expected<SerializeU64Work, core::Error> {
    if (repetitions.value() > maximum_repetitions()) return std::unexpected{error("encoded_work_overflow")};
    return SerializeU64Work{repetitions, core::WorkCount::from_base_units(repetitions.value() * element_count().value()),
        core::ByteCount::from_base_units(repetitions.value() * logical_bytes().value())};
}

auto prepare_serialize_u64_reference(const SerializeU64Config& config, std::span<const std::byte> input,
    std::span<std::byte> output) -> std::expected<SerializeU64Reference, core::Error> {
    if (input.size() < config.native_bytes() || output.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    const auto source = prepare_sum_u64_reference(config.input_configuration(), input);
    if (!source) return std::unexpected{source.error()};
    std::uint64_t checksum{};
    for (std::size_t offset{}; offset < config.native_bytes(); offset += 8U) {
        std::ranges::reverse_copy(input.subspan(offset, 8U), output.subspan(offset, 8U).begin());
        checksum += std::to_integer<unsigned>(output[offset]);
    }
    std::ranges::fill(output.subspan(config.native_bytes()), serialize_u64_padding_sentinel);
    return SerializeU64Reference{config, source->input_digest(), core::sha256(output.first(config.native_bytes())), checksum};
}

auto reset_serialize_u64_output(const SerializeU64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("output_too_small")};
    for (std::size_t index{}; index < config.element_count().value(); ++index) {
        const auto word = detail::mix64_v1(config.input_seed() + index);
        for (unsigned byte{}; byte < 8U; ++byte)
            output[8U * index + byte] = ~static_cast<std::byte>((word >> (56U - 8U * byte)) & 0xffU);
    }
    std::ranges::fill(output.subspan(config.native_bytes()), serialize_u64_padding_sentinel);
    return {};
}

auto verify_serialize_u64_output(const SerializeU64Reference& reference, std::span<const std::byte> output)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& config = reference.configuration();
    if (output.size() < config.native_bytes()) return std::unexpected{error("output_too_small")};
    const auto digest = core::sha256(output.first(config.native_bytes()));
    const auto padding = std::ranges::all_of(output.subspan(config.native_bytes()),
        [](std::byte value) { return value == serialize_u64_padding_sentinel; });
    return XorU64Verification{config.element_count(), reference.expected_output_digest(), digest,
                              digest == reference.expected_output_digest(), padding};
}
} // namespace epycbench::kernel
