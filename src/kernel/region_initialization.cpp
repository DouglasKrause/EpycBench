#include "epycbench/kernel/region_initialization.hpp"

#include "kernel/region_initialization_access.hpp"
#include "kernel/bounded_index.hpp"
#include "kernel/canonical_latency_permutation.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::kernel {
namespace {

[[nodiscard]] auto initializer_error(std::string_view code,
                                     std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Kernel region initialization failed",
            .operation = "kernel.region_initializer",
            .os_error = std::nullopt,
            .context = nlohmann::json{{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto invalid(std::string_view classification) -> core::Error {
    return initializer_error("kernel.region_initializer.invalid_argument", classification);
}

[[nodiscard]] auto arithmetic(std::string_view classification) -> core::Error {
    return initializer_error("kernel.region_initializer.arithmetic_failed", classification);
}

[[nodiscard]] auto content_invalid(std::string_view classification) -> core::Error {
    return initializer_error("kernel.region_initializer.content_invalid", classification);
}

[[nodiscard]] auto contained_standard() -> core::Error {
    return initializer_error("kernel.region_initializer.contained_exception",
                             "contained_exception");
}

[[nodiscard]] auto contained_unknown() -> core::Error {
    return initializer_error("kernel.region_initializer.contained_unknown_failure",
                             "contained_unknown_failure");
}

[[nodiscard]] auto checked_size(std::uint64_t value) noexcept -> std::optional<std::size_t> {
    if (value > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] auto build_successors(const std::vector<std::uint64_t>& permutation) noexcept
    -> std::expected<std::vector<std::uint64_t>, core::Error> {
    try {
        std::vector<std::uint64_t> successors(permutation.size());
        for (std::size_t position = 0; position + 1U < permutation.size(); ++position) {
            successors[static_cast<std::size_t>(permutation[position])] =
                permutation[position + 1U];
        }
        successors[static_cast<std::size_t>(permutation.back())] = permutation.front();
        return successors;
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

[[nodiscard]] auto validate_cycle(const std::vector<std::uint64_t>& successors) noexcept
    -> bool {
    try {
        if (successors.size() < 2U) {
            return false;
        }
        std::vector<std::uint8_t> predecessors(successors.size(), 0U);
        for (std::size_t node = 0; node < successors.size(); ++node) {
            const auto successor = successors[node];
            if (successor >= successors.size() || successor == node) {
                return false;
            }
            auto& count = predecessors[static_cast<std::size_t>(successor)];
            if (count != 0U) {
                return false;
            }
            count = 1U;
        }
        if (!std::ranges::all_of(predecessors, [](std::uint8_t value) { return value == 1U; })) {
            return false;
        }
        std::vector<bool> visited(successors.size(), false);
        std::size_t node = 0;
        for (std::size_t visit = 0; visit < successors.size(); ++visit) {
            if (visited[node]) {
                return false;
            }
            visited[node] = true;
            node = static_cast<std::size_t>(successors[node]);
        }
        return node == 0U &&
               std::ranges::all_of(visited, [](bool value) { return value; });
    } catch (...) {
        return false;
    }
}

} // namespace

auto RegionInitializationSpec::bandwidth(
    SequentialReadBandwidthConfig configuration) noexcept -> RegionInitializationSpec {
    return {RegionContentFamily::sequential_read_bandwidth,
            configuration.working_set_per_worker(), std::nullopt};
}

auto RegionInitializationSpec::latency(DependentLoadLatencyConfig configuration,
                                       std::uint64_t resolved_permutation_seed) noexcept
    -> std::expected<RegionInitializationSpec, core::Error> {
    try {
        if (configuration.pointer_cycle_seed().mode() ==
                PointerCycleSeedMode::explicit_value &&
            configuration.pointer_cycle_seed().value() != resolved_permutation_seed) {
            return std::unexpected{invalid("resolved_seed_mismatch")};
        }
        return RegionInitializationSpec{RegionContentFamily::dependent_load_latency,
                                        configuration.working_set(),
                                        resolved_permutation_seed};
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

auto detail::RegionInitializationAccess::mix64(std::uint64_t value) noexcept -> std::uint64_t {
    return mix64_v1(value);
}

void detail::RegionInitializationAccess::store_little_endian(
    std::span<std::byte, 8> destination, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < destination.size(); ++index) {
        destination[index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

auto detail::RegionInitializationAccess::canonical_permutation(std::size_t node_count,
                                                               std::uint64_t seed) noexcept
    -> std::expected<std::vector<std::uint64_t>, core::Error> {
    try {
        if (node_count < 2U || node_count > std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected{invalid("node_count_invalid")};
        }
        auto permutation = detail::canonical_latency_permutation(node_count, seed);
        if (!permutation) {
            return std::unexpected{arithmetic("stream_exhausted")};
        }
        return std::move(*permutation);
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

auto detail::RegionInitializationAccess::bounded_draw_for_test(
    std::span<const std::uint64_t> draws, std::size_t& cursor,
    std::uint64_t bound) noexcept -> std::expected<std::uint64_t, core::Error> {
    if (bound == 0U) {
        return std::unexpected{invalid("bounded_draw_invalid")};
    }
    while (cursor < draws.size()) {
        const auto selected = detail::bounded_index(draws[cursor], bound);
        ++cursor;
        if (selected) {
            return *selected;
        }
    }
    return std::unexpected{arithmetic("stream_exhausted")};
}

auto PreparedRegionInitializer::prepare(RegionInitializationSpec specification) noexcept
    -> std::expected<PreparedRegionInitializer, core::Error> {
    try {
        const auto logical = specification.logical_bytes().value();
        if (logical == 0U || logical % 64U != 0U) {
            return std::unexpected{invalid("logical_size_invalid")};
        }
        if (!checked_size(logical)) {
            return std::unexpected{arithmetic("logical_size_invalid")};
        }

        auto span_failure = invalid("span_size_mismatch");
        auto alignment_failure = invalid("alignment_invalid");
        auto invariant_failure = content_invalid("prepared_content_invalid");

        if (specification.family() == RegionContentFamily::sequential_read_bandwidth) {
            if (specification.permutation_seed()) {
                return std::unexpected{invalid("content_profile_invalid")};
            }
            const auto words = logical / 8U;
            auto evidence = RegionContentEvidence{
                specification.family(), bandwidth_region_layout_profile,
                specification.logical_bytes(), words, std::nullopt, std::nullopt, true};
            return PreparedRegionInitializer{std::move(evidence), {}, std::move(span_failure),
                                             std::move(alignment_failure),
                                             std::move(invariant_failure)};
        }

        if (specification.family() != RegionContentFamily::dependent_load_latency ||
            !specification.permutation_seed() || logical < 128U) {
            return std::unexpected{invalid("content_profile_invalid")};
        }
        const auto nodes64 = logical / 64U;
        const auto nodes = checked_size(nodes64);
        if (!nodes || *nodes < 2U ||
            nodes64 > (std::numeric_limits<std::uint64_t>::max() - 6U) / 7U) {
            return std::unexpected{arithmetic("node_count_invalid")};
        }
        auto permutation = detail::RegionInitializationAccess::canonical_permutation(
            *nodes, *specification.permutation_seed());
        if (!permutation) {
            return std::unexpected{std::move(permutation.error())};
        }
        auto successors = build_successors(*permutation);
        if (!successors) {
            return std::unexpected{std::move(successors.error())};
        }
        if (!validate_cycle(*successors)) {
            return std::unexpected{content_invalid("cycle_invalid")};
        }
        auto evidence = RegionContentEvidence{
            specification.family(), latency_region_layout_profile,
            specification.logical_bytes(), std::nullopt, nodes64,
            specification.permutation_seed(), true};
        return PreparedRegionInitializer{std::move(evidence), std::move(*successors),
                                         std::move(span_failure),
                                         std::move(alignment_failure),
                                         std::move(invariant_failure)};
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

auto PreparedRegionInitializer::initialize(std::span<std::byte> logical_region) const noexcept
    -> std::expected<void, core::Error> {
    if (logical_region.size() != evidence_.logical_bytes().value()) {
        return std::unexpected{std::move(span_size_failure_)};
    }
    if (reinterpret_cast<std::uintptr_t>(logical_region.data()) % 64U != 0U) {
        return std::unexpected{std::move(alignment_failure_)};
    }

    if (evidence_.family() == RegionContentFamily::sequential_read_bandwidth) {
        const auto words = *evidence_.word_count();
        for (std::uint64_t word = 0; word < words; ++word) {
            const auto offset = static_cast<std::size_t>(word * 8U);
            detail::RegionInitializationAccess::store_little_endian(
                std::span<std::byte, 8>{logical_region.data() + offset, 8U},
                detail::RegionInitializationAccess::mix64(word));
        }
        return {};
    }

    if (evidence_.family() != RegionContentFamily::dependent_load_latency ||
        !evidence_.node_count() || successors_.size() != *evidence_.node_count()) {
        return std::unexpected{std::move(invariant_failure_)};
    }
    for (std::size_t node = 0; node < successors_.size(); ++node) {
        const auto node_offset = node * 64U;
        detail::RegionInitializationAccess::store_little_endian(
            std::span<std::byte, 8>{logical_region.data() + node_offset, 8U}, successors_[node]);
        for (std::size_t slot = 0; slot < 7U; ++slot) {
            const auto ordinal = static_cast<std::uint64_t>(node) * 7U + slot;
            detail::RegionInitializationAccess::store_little_endian(
                std::span<std::byte, 8>{logical_region.data() + node_offset + (slot + 1U) * 8U,
                                        8U},
                detail::RegionInitializationAccess::mix64(ordinal));
        }
    }
    return {};
}

} // namespace epycbench::kernel
