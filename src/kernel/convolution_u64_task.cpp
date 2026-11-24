#include "epycbench/kernel/convolution_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <array>
#include <functional>

namespace epycbench::kernel {
namespace {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.convolution_u64." + std::string{reason}},
            .message = "Five-tap convolution validation failed",
            .operation = "kernel.task.convolution_u64",
            .os_error = {},
            .context = {{"classification", reason}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
auto read(Input bytes, std::size_t index) noexcept -> std::uint64_t {
    std::uint64_t value{};
    for (unsigned i{}; i < 8U; ++i)
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[8U * index + i])} << (8U * i);
    return value;
}
void write(Output bytes, std::size_t index, std::uint64_t value) noexcept {
    for (unsigned i{}; i < 8U; ++i)
        bytes[8U * index + i] = static_cast<std::byte>((value >> (8U * i)) & 255U);
}
auto sample(const ConvolutionU64Config& c, std::uint64_t i) noexcept -> std::uint64_t {
    return detail::mix64_v1(c.input_seed() + i);
}
template <class Accept> void canonical_input(const ConvolutionU64Config& c, Accept accept) {
    accept(0U, convolution_u64_input_magic);
    accept(1U, c.sample_count());
    for (std::size_t k = 0; k < convolution_u64_taps.size(); ++k)
        accept(2U + k, convolution_u64_taps[k]);
    for (std::uint64_t i = 0; i < c.sample_count(); ++i)
        accept(convolution_u64_header_words + i, sample(c, i));
}
auto expected_sample(const ConvolutionU64Config& c, std::size_t i) noexcept -> std::uint64_t {
    std::uint64_t sum{};
    for (std::size_t k = 0; k < convolution_u64_taps.size(); ++k)
        sum += sample(c, i + convolution_u64_taps.size() - 1U - k) * convolution_u64_taps[k];
    return sum;
}
} // namespace

auto ConvolutionU64Config::create(std::uint64_t samples, std::uint64_t seed)
    -> std::expected<ConvolutionU64Config, core::Error> {
    if (samples < convolution_u64_taps.size() || samples > convolution_u64_maximum_samples)
        return std::unexpected{error("invalid_sample_count")};
    return ConvolutionU64Config{samples, seed};
}
auto ConvolutionU64Config::work(TaskRepetitionCount r) const
    -> std::expected<TaskWork, core::Error> {
    if (r.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return TaskWork::create(element_count(), r);
}
auto initialize_convolution_u64_input(const ConvolutionU64Config& c, Output buf)
    -> std::expected<void, core::Error> {
    if (buf.size() < c.logical_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_input(c, [&](auto i, auto value) { write(buf, i, value); });
    return {};
}
auto prepare_convolution_u64_reference(const ConvolutionU64Config& c, Input input, Output output)
    -> std::expected<ConvolutionU64Reference, core::Error> {
    if (input.size() < c.logical_bytes().value() || output.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) &&
        less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    bool matches = true;
    canonical_input(c, [&](auto i, auto value) { matches &= read(input, i) == value; });
    if (!matches)
        return std::unexpected{error("input_content_mismatch")};
    // Distribute each source value to its valid destinations. The measured modules gather by
    // output.
    std::ranges::fill(output.first(c.output_bytes().value()), std::byte{});
    for (std::size_t i = 0; i < c.sample_count(); ++i) {
        const auto value = read(input, convolution_u64_header_words + i);
        for (std::size_t k = 0; k < convolution_u64_taps.size(); ++k) {
            const auto shift = convolution_u64_taps.size() - 1U - k;
            if (i >= shift && i - shift < c.output_samples()) {
                const auto dest = i - shift;
                write(output, dest, read(output, dest) + value * convolution_u64_taps[k]);
            }
        }
    }
    std::uint64_t folded{};
    for (std::size_t i = 0; i < c.output_samples(); ++i)
        folded += read(output, i);
    std::ranges::fill(output.subspan(c.output_bytes().value()), convolution_u64_padding_sentinel);
    return ConvolutionU64Reference{c, core::sha256(input.first(c.logical_bytes().value())),
                                   core::sha256(output.first(c.output_bytes().value())), folded};
}
auto reset_convolution_u64_output(const ConvolutionU64Reference& ref, Output buf)
    -> std::expected<void, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    for (std::size_t i = 0; i < c.output_samples(); ++i)
        write(buf, i, ~expected_sample(c, i));
    std::ranges::fill(buf.subspan(c.output_bytes().value()), convolution_u64_padding_sentinel);
    return {};
}
auto verify_convolution_u64_output(const ConvolutionU64Reference& ref, Input buf)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    bool matches = true;
    for (std::size_t i = 0; i < c.output_samples(); ++i)
        matches &= read(buf, i) == expected_sample(c, i);
    const bool padding = std::ranges::all_of(buf.subspan(c.output_bytes().value()), [](auto value) {
        return value == convolution_u64_padding_sentinel;
    });
    return XorU64Verification{c.element_count(), ref.expected_output_digest(),
                              core::sha256(buf.first(c.output_bytes().value())), matches, padding};
}
} // namespace epycbench::kernel
