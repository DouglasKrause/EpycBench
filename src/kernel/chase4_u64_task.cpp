#include "epycbench/kernel/chase4_u64_task.hpp"
#include "kernel/canonical_latency_permutation.hpp"
#include <algorithm>
#include <new>
#include <stdexcept>

namespace epycbench::kernel {
namespace {
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.chase4_u64." + std::string{reason}},
            .message = "Four-chain dependent-load task validation failed",
            .operation = "kernel.task.chase4_u64",
            .os_error = {},
            .context = {{"classification", reason}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
auto permutation(const Chase4U64Config& config, std::uint64_t lane)
    -> std::expected<std::vector<std::uint64_t>, core::Error> {
    try {
        const auto seed =
            detail::mix64_v1(config.input_seed() + (lane + 1U) * chase4_u64_seed_stride);
        auto order = detail::canonical_latency_permutation(
            static_cast<std::size_t>(config.nodes_per_lane().value()), seed);
        if (!order)
            return std::unexpected{error("permutation_stream_exhausted")};
        return std::move(*order);
    } catch (const std::bad_alloc&) {
        return std::unexpected{error("allocation_failure")};
    } catch (const std::length_error&) {
        return std::unexpected{error("allocation_failure")};
    }
}
void write(std::span<std::byte> input, std::uint64_t node, std::uint64_t successor) {
    const auto offset = static_cast<std::size_t>(node * chase4_u64_node_bytes);
    for (unsigned byte{}; byte < 8U; ++byte)
        input[offset + byte] = static_cast<std::byte>((successor >> (byte * 8U)) & 0xffU);
}
} // namespace

auto Chase4U64Config::create(std::uint64_t nodes, std::uint64_t seed)
    -> std::expected<Chase4U64Config, core::Error> {
    if (!nodes || nodes > chase4_u64_maximum_nodes_per_lane)
        return std::unexpected{error("node_count_out_of_range")};
    auto count = ElementCount::create(nodes);
    if (!count)
        return std::unexpected{error("node_count_out_of_range")};
    return Chase4U64Config{*count, seed};
}
auto Chase4U64Config::work(TaskRepetitionCount count) const
    -> std::expected<Chase4U64Work, core::Error> {
    if (count.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return Chase4U64Work{
        count, core::WorkCount::from_base_units(element_count().value() * count.value())};
}
auto initialize_chase4_u64_input(const Chase4U64Config& config, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    std::fill(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(config.native_bytes()),
              std::byte{});
    const auto nodes = config.nodes_per_lane().value();
    for (std::uint64_t lane{}; lane < 4U; ++lane) {
        auto order = permutation(config, lane);
        if (!order)
            return std::unexpected{std::move(order.error())};
        for (std::size_t position{}; position < order->size(); ++position)
            write(input, lane * nodes + (*order)[position],
                  lane * nodes + (*order)[(position + 1U) % order->size()]);
    }
    return {};
}
auto prepare_chase4_u64_reference(const Chase4U64Config& config, std::span<const std::byte> input)
    -> std::expected<Chase4U64Reference, core::Error> {
    if (input.size() < config.native_bytes())
        return std::unexpected{error("storage_too_small")};
    std::array<std::uint64_t, 4> terminals{};
    std::uint64_t answer{};
    const auto nodes = config.nodes_per_lane().value();
    for (std::uint64_t lane{}; lane < 4U; ++lane) {
        auto order = permutation(config, lane);
        if (!order)
            return std::unexpected{std::move(order.error())};
        for (std::size_t position{}; position < order->size(); ++position) {
            const auto local = (*order)[position];
            const auto expected = lane * nodes + (*order)[(position + 1U) % order->size()];
            const auto offset = static_cast<std::size_t>((lane * nodes + local) * 64U);
            std::uint64_t observed{};
            for (unsigned byte{}; byte < 8U; ++byte)
                observed |= std::uint64_t{std::to_integer<unsigned char>(input[offset + byte])}
                            << (byte * 8U);
            if (observed != expected ||
                !std::all_of(input.begin() + static_cast<std::ptrdiff_t>(offset + 8U),
                             input.begin() + static_cast<std::ptrdiff_t>(offset + 64U),
                             [](std::byte byte) { return byte == std::byte{}; }))
                return std::unexpected{error("input_content_mismatch")};
            if (local == 0U)
                terminals[static_cast<std::size_t>(lane)] = expected;
        }
        answer += terminals[static_cast<std::size_t>(lane)] * (2U * lane + 1U);
    }
    return Chase4U64Reference{config, core::sha256(input.first(config.native_bytes())), terminals,
                              answer};
}
} // namespace epycbench::kernel
