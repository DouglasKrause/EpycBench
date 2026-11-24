#include "epycbench/kernel/norm_l1_f64_task.hpp"
#include "kernel/dyadic_f64_input.hpp"

#include <limits>

namespace epycbench::kernel {
namespace {
auto error(std::string_view classification, std::string_view parameter = {}) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    if (!parameter.empty()) context["parameter"] = parameter;
    return {.category = core::ErrorCategory::kernel,
        .code = core::StableErrorCode{"kernel.task.norm_l1_f64." + std::string{classification}},
        .message = "Exact binary64 L1 norm task validation failed",
        .operation = "kernel.task.norm_l1_f64", .os_error = {}, .context = std::move(context),
        .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_experiment};
}
} // namespace

auto NormL1F64Config::create(std::uint64_t count, std::uint64_t seed) -> std::expected<NormL1F64Config, core::Error> {
    if (count == 0U || count > norm_l1_f64_maximum_elements)
        return std::unexpected{error("element_count_out_of_range", "element_count")};
    if (count > std::numeric_limits<std::size_t>::max() / 8U)
        return std::unexpected{error("storage_overflow", "element_count")};
    auto elements = ElementCount::create(count);
    if (!elements) return std::unexpected{std::move(elements.error())};
    return NormL1F64Config{*elements, seed, core::ByteCount::from_base_units(count * 8U)};
}
auto NormL1F64Config::native_bytes() const noexcept -> std::size_t {
    return static_cast<std::size_t>(bytes_.value());
}
auto NormL1F64Config::maximum_repetitions() const noexcept -> std::uint64_t {
    return std::numeric_limits<std::uint64_t>::max() / elements_.value();
}
auto initialize_norm_l1_f64_input(const NormL1F64Config& configuration, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i)
        detail::write_dyadic_word(input, static_cast<std::size_t>(i * 8U), detail::dyadic_bits(detail::dyadic_numerator(configuration.input_seed(), i)));
    return {};
}
auto prepare_norm_l1_f64_reference(const NormL1F64Config& configuration, std::span<const std::byte> input)
    -> std::expected<NormL1F64Reference, core::Error> {
    if (input.size() < configuration.native_bytes()) return std::unexpected{error("storage_too_small")};
    std::uint64_t sum{};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i) {
        const auto value = detail::dyadic_numerator(configuration.input_seed(), i);
        if (detail::read_dyadic_word(input, static_cast<std::size_t>(i * 8U)) != detail::dyadic_bits(value))
            return std::unexpected{error("input_content_mismatch")};
        sum += detail::dyadic_magnitude(value);
    }
    return NormL1F64Reference{configuration, core::sha256(input.first(configuration.native_bytes())), sum, detail::dyadic_magnitude_bits(sum)};
}
} // namespace epycbench::kernel
