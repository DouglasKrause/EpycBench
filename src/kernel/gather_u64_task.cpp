#include "epycbench/kernel/gather_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <limits>

namespace epycbench::kernel {
namespace {
static_assert(gather_u64_header_bytes + 16U * gather_u64_maximum_count <=
              static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()));
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.gather_u64." + std::string{reason}},
            .message = "Indexed gather task validation failed",
            .operation = "kernel.task.gather_u64",
            .os_error = {},
            .context = {{"classification", reason}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
void write(std::span<std::byte> bytes, std::uint64_t slot, std::uint64_t word) {
    for (unsigned b{}; b < 8U; ++b)
        bytes[static_cast<std::size_t>(slot * 8U) + b] =
            static_cast<std::byte>((word >> (8U * b)) & 0xffU);
}
auto read(std::span<const std::byte> bytes, std::uint64_t slot) -> std::uint64_t {
    std::uint64_t word{};
    for (unsigned b{}; b < 8U; ++b)
        word |= std::uint64_t{std::to_integer<unsigned char>(
                    bytes[static_cast<std::size_t>(slot * 8U) + b])}
                << (8U * b);
    return word;
}
auto index(const GatherU64Config& c, std::uint64_t position) noexcept -> std::uint64_t {
    if (c.cache_set_pattern()) {
        const auto pages = (gather_u64_header_bytes + 8U * c.value_count().value()) / gather_set_page_bytes;
        const auto stride = detail::mix64_v1(c.input_seed() ^ gather_u64_index_seed_xor) | 1U;
        const auto start = detail::mix64_v1(c.input_seed() ^ gather_u64_cluster_seed_xor);
        if (c.matched_page_pattern()) {
            const auto logical_line = (start + position * stride) % gather_matched_page_lines;
            const auto line_within_page = logical_line % 64U;
            const auto group = logical_line / 64U;
            const auto page = c.index_pattern() == GatherU64IndexPattern::pages_compact ? group : 4U * line_within_page + group;
            // Byte 32 in each line also fits the first page's 32-byte header.
            const auto address = page * gather_set_page_bytes + line_within_page * gather_set_line_bytes + 32U;
            return (address - gather_u64_header_bytes) / sizeof(std::uint64_t);
        }
        const auto page = (start + position * stride) % pages;
        const auto line = c.index_pattern() == GatherU64IndexPattern::set_fixed ? 1U :
                          (page + 1U) % (gather_set_page_bytes / gather_set_line_bytes);
        return (page * gather_set_page_bytes + line * gather_set_line_bytes - gather_u64_header_bytes) / 8U;
    }
    const auto random = detail::mix64_v1((c.input_seed() ^ gather_u64_index_seed_xor) + position);
    if (c.index_pattern() == GatherU64IndexPattern::spread)
        return random % c.value_count().value();
    const auto width = std::min(c.value_count().value(), gather_u64_cluster_values);
    const auto start = detail::mix64_v1(c.input_seed() ^ gather_u64_cluster_seed_xor) %
                       (c.value_count().value() - width + 1U);
    return start + random % width;
}
auto table_value(const GatherU64Config& c, std::uint64_t i) noexcept -> std::uint64_t {
    if (c.matched_page_pattern()) {
        const auto offset = gather_u64_header_bytes + sizeof(std::uint64_t) * i;
        const auto page = offset / gather_set_page_bytes;
        const auto line = (offset % gather_set_page_bytes) / gather_set_line_bytes;
        return detail::mix64_v1(c.input_seed() + (page % 4U) * 64U + line);
    }
    const auto identity = c.cache_set_pattern() ? (gather_u64_header_bytes + 8U * i) / gather_set_page_bytes : i;
    return detail::mix64_v1(c.input_seed() + identity);
}
} // namespace

auto gather_u64_index_pattern_token(GatherU64IndexPattern pattern) noexcept -> std::string_view {
    switch (pattern) {
    case GatherU64IndexPattern::clustered:
        return "clustered";
    case GatherU64IndexPattern::spread:
        return "spread";
    case GatherU64IndexPattern::set_fixed:
        return "set-fixed";
    case GatherU64IndexPattern::set_cycle:
        return "set-cycle";
    case GatherU64IndexPattern::pages_compact:
        return "pages-compact";
    case GatherU64IndexPattern::pages_spread:
        return "pages-spread";
    }
    return {};
}
auto parse_gather_u64_index_pattern(std::string_view token) noexcept -> std::optional<GatherU64IndexPattern> {
    for (const auto pattern : {GatherU64IndexPattern::clustered, GatherU64IndexPattern::spread,
                              GatherU64IndexPattern::set_fixed, GatherU64IndexPattern::set_cycle,
                              GatherU64IndexPattern::pages_compact, GatherU64IndexPattern::pages_spread}) {
        if (gather_u64_index_pattern_token(pattern) == token) return pattern;
    }
    return {};
}
auto gather_u64_implementation_matches(const GatherU64Config& config, std::string_view implementation) noexcept -> bool {
    if (config.matched_page_pattern())
        return implementation == gather_matched_pages_u64_single_implementation_id || implementation == gather_matched_pages_u64_four_implementation_id;
    if (config.cache_set_pattern())
        return implementation == gather_cache_sets_u64_single_implementation_id || implementation == gather_cache_sets_u64_four_implementation_id;
    return implementation == gather_u64_single_implementation_id || implementation == gather_u64_four_implementation_id;
}
auto GatherU64Config::create(std::uint64_t values, std::uint64_t indices,
                             GatherU64IndexPattern pattern, std::uint64_t seed)
    -> std::expected<GatherU64Config, core::Error> {
    if (!values || !indices || values > gather_u64_maximum_count ||
        indices > gather_u64_maximum_count)
        return std::unexpected{error("count_out_of_range")};
    if (gather_u64_index_pattern_token(pattern).empty())
        return std::unexpected{error("unknown_index_pattern")};
    if ((pattern == GatherU64IndexPattern::pages_compact || pattern == GatherU64IndexPattern::pages_spread) &&
        !valid_gather_matched_page_shape(values, indices))
        return std::unexpected{error("invalid_matched_page_shape")};
    if ((pattern == GatherU64IndexPattern::set_fixed || pattern == GatherU64IndexPattern::set_cycle) &&
        !valid_gather_cache_set_table(values))
        return std::unexpected{error("invalid_page_set_table_extent")};
    return GatherU64Config{*ElementCount::create(values), *ElementCount::create(indices), pattern,
                           seed};
}
auto GatherU64Config::work(TaskRepetitionCount count) const
    -> std::expected<GatherU64Work, core::Error> {
    if (count.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return GatherU64Work{count, core::WorkCount::from_base_units(indices_.value() * count.value())};
}
auto initialize_gather_u64_input(const GatherU64Config& c, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < c.native_bytes())
        return std::unexpected{error("storage_too_small")};
    write(input, 0U, gather_u64_input_magic);
    write(input, 1U, c.value_count().value());
    write(input, 2U, c.index_count().value());
    write(input, 3U, static_cast<std::uint64_t>(c.index_pattern()));
    for (std::uint64_t i{}; i < c.value_count().value(); ++i)
        write(input, 4U + i, table_value(c, i));
    for (std::uint64_t i{}; i < c.index_count().value(); ++i)
        write(input, 4U + c.value_count().value() + i, index(c, i));
    return {};
}
auto prepare_gather_u64_reference(const GatherU64Config& c, std::span<const std::byte> input)
    -> std::expected<GatherU64Reference, core::Error> {
    if (input.size() < c.native_bytes())
        return std::unexpected{error("storage_too_small")};
    if (read(input, 0U) != gather_u64_input_magic || read(input, 1U) != c.value_count().value() ||
        read(input, 2U) != c.index_count().value() ||
        read(input, 3U) != static_cast<std::uint64_t>(c.index_pattern()))
        return std::unexpected{error("header_mismatch")};
    for (std::uint64_t i{}; i < c.value_count().value(); ++i)
        if (read(input, 4U + i) != table_value(c, i))
            return std::unexpected{error("value_mismatch")};
    std::uint64_t answer{};
    for (std::uint64_t i{}; i < c.index_count().value(); ++i) {
        const auto expected_index = index(c, i);
        if (read(input, 4U + c.value_count().value() + i) != expected_index)
            return std::unexpected{error("index_mismatch")};
        answer += table_value(c, expected_index);
    }
    return GatherU64Reference{c, core::sha256(input.first(c.native_bytes())), answer};
}
} // namespace epycbench::kernel
