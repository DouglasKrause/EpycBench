#include "epycbench/kernel/varint14_task.hpp"
#include "kernel/canonical_latency_permutation.hpp"
#include "kernel/mix64.hpp"
#include <array>
#include <new>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace epycbench::kernel {
namespace {
auto error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.varint14." + std::string{classification}},
            .message = "Canonical varint14 task validation failed",
            .operation = "kernel.task.varint14",
            .os_error = {},
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
auto item_value(const Varint14Config& config, std::uint64_t item) noexcept -> std::uint64_t {
    const auto bits = detail::mix64_v1(config.input_seed() + item);
    return item < config.record_count().value() / 2U ? bits & 127U : 128U + bits % 16256U;
}
auto order(const Varint14Config& config) -> std::expected<std::vector<std::uint64_t>, core::Error> {
    try {
        if (config.distribution() == Varint14Distribution::shuffled) {
            auto permutation = detail::canonical_latency_permutation(
                static_cast<std::size_t>(config.record_count().value()),
                detail::mix64_v1(config.input_seed() ^ varint14_shuffle_seed_xor));
            if (!permutation)
                return std::unexpected{error("permutation_stream_exhausted")};
            return std::move(*permutation);
        }
        std::vector<std::uint64_t> result(static_cast<std::size_t>(config.record_count().value()));
        std::iota(result.begin(), result.end(), UINT64_C(0));
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected{error("allocation")};
    }
}
auto encoded(std::uint64_t value) noexcept -> std::array<std::byte, 2> {
    return {static_cast<std::byte>((value & 127U) | (value >= 128U ? 128U : 0U)),
            static_cast<std::byte>(value >> 7U)};
}
} // namespace
auto varint14_distribution_token(Varint14Distribution value) noexcept -> std::string_view {
    if (value == Varint14Distribution::grouped)
        return "grouped";
    if (value == Varint14Distribution::shuffled)
        return "shuffled";
    return {};
}
auto Varint14Config::create(std::uint64_t count, Varint14Distribution distribution,
                            std::uint64_t seed) -> std::expected<Varint14Config, core::Error> {
    if (count < 2U || count > varint14_maximum_records || count % 2U)
        return std::unexpected{error("record_count_out_of_range")};
    if (varint14_distribution_token(distribution).empty())
        return std::unexpected{error("distribution_invalid")};
    auto records = ElementCount::create(count);
    if (!records)
        return std::unexpected{error("record_count_out_of_range")};
    return Varint14Config{*records, distribution, seed};
}
auto Varint14Config::work(TaskRepetitionCount count) const
    -> std::expected<Varint14Work, core::Error> {
    if (count.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return Varint14Work{count, core::WorkCount::from_base_units(records_.value() * count.value()),
                        core::WorkCount::from_base_units(logical_bytes().value() * count.value())};
}
auto initialize_varint14_input(const Varint14Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    auto items = order(config);
    if (!items)
        return std::unexpected{std::move(items.error())};
    std::size_t cursor{};
    for (const auto item : *items) {
        const auto value = item_value(config, item);
        const auto bytes = encoded(value);
        input[cursor++] = bytes[0];
        if (value >= 128U)
            input[cursor++] = bytes[1];
    }
    return {};
}
auto prepare_varint14_reference(const Varint14Config& config, std::span<const std::byte> input)
    -> std::expected<Varint14Reference, core::Error> {
    if (input.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    auto items = order(config);
    if (!items)
        return std::unexpected{std::move(items.error())};
    std::size_t cursor{};
    for (const auto item : *items) {
        const auto value = item_value(config, item);
        const auto bytes = encoded(value);
        if (input[cursor++] != bytes[0] || (value >= 128U && input[cursor++] != bytes[1]))
            return std::unexpected{error("input_content_mismatch")};
    }
    // The answer does not parse the input or follow its order/branch history.
    std::uint64_t answer{};
    for (std::uint64_t item{}; item < config.record_count().value(); ++item)
        answer += item_value(config, item);
    return Varint14Reference{config, core::sha256(input.first(config.native_bytes())), answer};
}
} // namespace epycbench::kernel
