#include "epycbench/kernel/affine_f64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <limits>

namespace epycbench::kernel {
namespace {
constexpr auto maximum_elements() noexcept -> std::uint64_t {
    constexpr auto wide = std::numeric_limits<std::uint64_t>::max() / 16U;
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t))
        return std::numeric_limits<std::size_t>::max() / 16U;
    return wide;
}
auto error(std::string_view classification, std::string_view parameter = {}) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    if (!parameter.empty()) context["parameter"] = parameter;
    return {.category = core::ErrorCategory::kernel,
        .code = core::StableErrorCode{"kernel.task.affine_f64." + std::string{classification}},
        .message = "Exact binary64 affine task validation failed",
        .operation = "kernel.task.affine_f64", .os_error = {}, .context = std::move(context),
        .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_experiment};
}
auto numerator(const AffineF64Config& configuration, std::uint64_t index) noexcept -> std::int64_t {
    const auto value = detail::mix64_v1(configuration.input_seed() + index) & UINT64_C(0x7ffff);
    return static_cast<std::int64_t>(2U * value) - INT64_C(524287);
}
// Exact normal binary64 encoding of the bounded dyadic numerator/2^scale.
// No host floating operation, rounding mode or extended precision participates.
auto dyadic_bits(std::int64_t value, unsigned scale) noexcept -> std::uint64_t {
    if (value == 0) return 0U;
    const auto magnitude = static_cast<std::uint64_t>(value < 0 ? -value : value);
    const auto top = static_cast<unsigned>(std::bit_width(magnitude)) - 1U;
    const auto exponent = static_cast<int>(top) - static_cast<int>(scale) + 1023;
    const auto fraction = (magnitude << (52U - top)) & UINT64_C(0x000fffffffffffff);
    return (value < 0 ? UINT64_C(0x8000000000000000) : 0U) |
        (static_cast<std::uint64_t>(exponent) << 52U) | fraction;
}
auto input_bits(const AffineF64Config& configuration, std::uint64_t index) noexcept -> std::uint64_t {
    return dyadic_bits(numerator(configuration, index), 10U);
}
auto output_bits(const AffineF64Config& configuration, std::uint64_t index) noexcept -> std::uint64_t {
    return dyadic_bits(INT64_C(2199023255552) + numerator(configuration, index) - 512, 42U);
}
auto read(std::span<const std::byte> bytes, std::size_t offset) noexcept -> std::uint64_t {
    std::uint64_t word{};
    for (unsigned i{}; i < 8U; ++i)
        word |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + i])} << (i * 8U);
    return word;
}
void write(std::span<std::byte> bytes, std::size_t offset, std::uint64_t word) noexcept {
    for (unsigned i{}; i < 8U; ++i)
        bytes[offset + i] = static_cast<std::byte>((word >> (i * 8U)) & 0xffU);
}
void padding(const AffineF64Config& configuration, std::span<std::byte> bytes) noexcept {
    std::ranges::fill(bytes.subspan(configuration.native_bytes()), affine_f64_padding_sentinel);
}
} // namespace

auto affine_f64_maximum_elements() noexcept -> std::uint64_t { return maximum_elements(); }
auto AffineF64Config::create(std::uint64_t count, std::uint64_t seed)
    -> std::expected<AffineF64Config, core::Error> {
    if (count == 0U) return std::unexpected{error("zero_element_count", "element_count")};
    if (count > maximum_elements()) return std::unexpected{error("combined_storage_overflow", "element_count")};
    auto elements = ElementCount::create(count);
    if (!elements) return std::unexpected{std::move(elements.error())};
    return AffineF64Config{*elements, seed, core::ByteCount::from_base_units(count * 8U)};
}
auto AffineF64Config::native_bytes() const noexcept -> std::size_t {
    return static_cast<std::size_t>(bytes_.value());
}
auto AffineF64Config::maximum_repetitions() const noexcept -> std::uint64_t {
    return std::numeric_limits<std::uint64_t>::max() / elements_.value();
}
auto initialize_affine_f64_input(const AffineF64Config& configuration, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i)
        write(input, static_cast<std::size_t>(i * 8U), input_bits(configuration, i));
    return {};
}
auto prepare_affine_f64_reference(const AffineF64Config& configuration,
    std::span<const std::byte> input, std::span<std::byte> output)
    -> std::expected<AffineF64Reference, core::Error> {
    if (input.size() < configuration.native_bytes() || output.size() < configuration.native_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i)
        if (read(input, static_cast<std::size_t>(i * 8U)) != input_bits(configuration, i))
            return std::unexpected{error("input_content_mismatch")};
    std::uint64_t answer{};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i) {
        const auto expected = output_bits(configuration, i);
        write(output, static_cast<std::size_t>(i * 8U), expected); answer += expected;
    }
    padding(configuration, output);
    return AffineF64Reference{configuration, core::sha256(input.first(configuration.native_bytes())),
        core::sha256(output.first(configuration.native_bytes())), answer};
}
auto reset_affine_f64_output(const AffineF64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& configuration = reference.configuration();
    if (output.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i)
        write(output, static_cast<std::size_t>(i * 8U), ~output_bits(configuration, i));
    padding(configuration, output); return {};
}
auto verify_affine_f64_output(const AffineF64Reference& reference, std::span<const std::byte> output)
    -> std::expected<AffineF64Verification, core::Error> {
    const auto& configuration = reference.configuration();
    if (output.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    bool content = true;
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i)
        content = (read(output, static_cast<std::size_t>(i * 8U)) == output_bits(configuration, i)) && content;
    const auto intact = std::ranges::all_of(output.subspan(configuration.native_bytes()),
        [](std::byte value) { return value == affine_f64_padding_sentinel; });
    return AffineF64Verification{configuration.element_count(), reference.expected_output_digest(),
        core::sha256(output.first(configuration.native_bytes())), content, intact};
}
} // namespace epycbench::kernel
