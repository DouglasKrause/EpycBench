#include "epycbench/kernel/dot_f64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace epycbench::kernel {
namespace {
auto error(std::string_view classification, std::string_view parameter = {}) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    if (!parameter.empty()) context["parameter"] = parameter;
    return {.category = core::ErrorCategory::kernel,
        .code = core::StableErrorCode{"kernel.task.dot_f64." + std::string{classification}},
        .message = "Exact binary64 dot-product task validation failed",
        .operation = "kernel.task.dot_f64", .os_error = {}, .context = std::move(context),
        .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_experiment};
}
auto numerator(std::uint64_t seed, std::uint64_t index) noexcept -> std::int64_t {
    return static_cast<std::int64_t>(detail::mix64_v1(seed + index) & UINT64_C(65535)) - 32768;
}
auto dyadic_bits(std::int64_t value, unsigned scale) noexcept -> std::uint64_t {
    if (value == 0) return 0U;
    const auto magnitude = static_cast<std::uint64_t>(value < 0 ? -value : value);
    const auto top = static_cast<unsigned>(std::bit_width(magnitude)) - 1U;
    const auto exponent = static_cast<int>(top) - static_cast<int>(scale) + 1023;
    return (value < 0 ? UINT64_C(0x8000000000000000) : 0U) |
        (static_cast<std::uint64_t>(exponent) << 52U) |
        ((magnitude << (52U - top)) & UINT64_C(0x000fffffffffffff));
}
void write(std::span<std::byte> bytes, std::size_t offset, std::uint64_t word) noexcept {
    for (unsigned byte{}; byte < 8U; ++byte)
        bytes[offset + byte] = static_cast<std::byte>((word >> (byte * 8U)) & 0xffU);
}
auto read(std::span<const std::byte> bytes, std::size_t offset) noexcept -> std::uint64_t {
    std::uint64_t word{};
    for (unsigned byte{}; byte < 8U; ++byte)
        word |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + byte])} << (byte * 8U);
    return word;
}
} // namespace

auto DotF64Config::create(std::uint64_t count) -> std::expected<DotF64Config, core::Error> {
    if (count == 0U || count > dot_f64_maximum_elements)
        return std::unexpected{error("element_count_out_of_range", "element_count")};
    if (count > std::numeric_limits<std::size_t>::max() / 16U)
        return std::unexpected{error("storage_overflow", "element_count")};
    auto elements = ElementCount::create(count);
    if (!elements) return std::unexpected{std::move(elements.error())};
    return DotF64Config{*elements, core::ByteCount::from_base_units(count * 16U)};
}
auto DotF64Config::native_bytes() const noexcept -> std::size_t { return static_cast<std::size_t>(bytes_.value()); }
auto DotF64Config::maximum_repetitions() const noexcept -> std::uint64_t {
    return std::numeric_limits<std::uint64_t>::max() / elements_.value();
}
auto initialize_dot_f64_input(const DotF64Config& configuration, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i) {
        write(input, static_cast<std::size_t>(i * 16U), dyadic_bits(numerator(dot_f64_x_seed, i), 15U));
        write(input, static_cast<std::size_t>(i * 16U + 8U), dyadic_bits(numerator(dot_f64_y_seed, i), 15U));
    }
    return {};
}
auto prepare_dot_f64_reference(const DotF64Config& configuration, std::span<const std::byte> input)
    -> std::expected<DotF64Reference, core::Error> {
    if (input.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    std::int64_t sum{};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i) {
        const auto x = numerator(dot_f64_x_seed, i), y = numerator(dot_f64_y_seed, i);
        const auto offset = static_cast<std::size_t>(i * 16U);
        if (read(input, offset) != dyadic_bits(x, 15U) || read(input, offset + 8U) != dyadic_bits(y, 15U))
            return std::unexpected{error("input_content_mismatch")};
        sum += x * y;
    }
    return DotF64Reference{configuration, core::sha256(input.first(configuration.native_bytes())), sum, dyadic_bits(sum, 30U)};
}
auto replay_dot_f64_reference(const DotF64Config& configuration, core::ByteCount maximum_generated_bytes)
    -> std::expected<DotF64Reference, core::Error> {
    static_assert(dot_f64_maximum_elements <= core::Sha256Stream::maximum_input_bytes / 16U);
    if (configuration.logical_bytes() > maximum_generated_bytes)
        return std::unexpected{error("reference_capacity")};

    core::Sha256Stream stream;
    constexpr auto chunk_bytes = 4096U;
    std::array<std::byte, chunk_bytes> chunk;
    constexpr std::uint64_t elements_per_chunk = chunk_bytes / 16U;
    const auto elements = configuration.element_count().value();
    std::int64_t sum{};
    for (std::uint64_t first{}; first < elements;) {
        const auto count = std::min(elements_per_chunk, elements - first);
        for (std::uint64_t index{}; index < count; ++index) {
            const auto x = numerator(dot_f64_x_seed, first + index);
            const auto y = numerator(dot_f64_y_seed, first + index);
            const auto offset = static_cast<std::size_t>(index * 16U);
            write(chunk, offset, dyadic_bits(x, 15U));
            write(chunk, offset + 8U, dyadic_bits(y, 15U));
            sum += x * y;
        }
        // The checked Dot configuration is always within the SHA-256 length limit.
        static_cast<void>(stream.append(std::span{chunk}.first(static_cast<std::size_t>(count * 16U))));
        first += count;
    }
    return DotF64Reference{configuration, stream.digest(), sum, dyadic_bits(sum, 30U)};
}
} // namespace epycbench::kernel
