#include "kernel/gather_inspection.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::kernel::detail::gather_inspection {

namespace {
auto value_offset(std::span<const std::byte> input, std::size_t offset) -> std::uint64_t {
    std::uint64_t index{};
    std::memcpy(&index, input.data() + offset, sizeof(index));
    if constexpr (std::endian::native == std::endian::big) index = std::byteswap(index);
    return kernel::gather_u64_header_bytes + sizeof(index) * index;
}
template<class Project>
auto inspect_input(const kernel::GatherU64Config& config, std::uint64_t page, Project project)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    if (!page || page % sizeof(std::uint64_t) != 0U) {
        return std::unexpected{core::Error{
            .category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.cli.gather_pages.profile"},
            .message = "Gather page inspection requires one v16 Gather comparison with word-aligned base pages",
            .operation = "scenario.resolve", .os_error = {}, .context = nlohmann::json::object(),
            .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_command}};
    }
    std::vector<std::byte> input(config.native_bytes());
    const auto initialized = kernel::initialize_gather_u64_input(config, input);
    if (!initialized) return std::unexpected{initialized.error()};
    const auto reference = kernel::prepare_gather_u64_reference(config, input);
    if (!reference) return std::unexpected{reference.error()};
    const auto index_offset = kernel::gather_u64_header_bytes + 8U * config.value_count().value();
    return project(std::span<const std::byte>{input}, reference->input_digest(), index_offset);
}
} // namespace

auto project_gather_pages(const kernel::GatherU64Config& config, std::uint64_t page, std::string_view workload_id)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    return inspect_input(config, page, [&](auto input, const auto& digest, std::size_t index_offset) {
        const auto first_index_page = index_offset / page;
        const auto last_index_page = (input.size() - 1U) / page;
        std::vector<std::uint8_t> touched(last_index_page + 1U);
        std::optional<std::uint64_t> previous;
        std::uint64_t changes{};
        core::Sha256Stream order_digest;
        for (std::size_t offset = index_offset; offset < input.size(); offset += 8U) {
            const auto value_page = value_offset(input, offset) / page;
            touched[value_page] = 1U;
            if (previous && *previous != value_page) ++changes;
            previous = value_page;
            auto encoded = value_page;
            if constexpr (std::endian::native == std::endian::big) encoded = std::byteswap(encoded);
            // The Gather profile caps this ordered stream at 8 MiB.
            static_cast<void>(order_digest.append(std::as_bytes(std::span{&encoded, 1U})));
        }
        const auto value_pages = std::ranges::count(touched, 1U);
        std::fill(touched.begin() + static_cast<std::ptrdiff_t>(first_index_page), touched.end(), 1U);
        return nlohmann::ordered_json{
            {"schema", "epycbench.gather-pages/v1"}, {"scope", "one-logical-repetition"},
            {"workload", workload_id}, {"input_sha256", digest.to_hex()},
            {"base_page_bytes", page}, {"logical_input_bytes", config.logical_bytes().value()},
            {"index_pattern", kernel::gather_u64_index_pattern_token(config.index_pattern())},
            {"indexed_lookups", config.index_count().value()}, {"distinct_value_pages", value_pages},
            {"index_stream_pages", last_index_page - first_index_page + 1U},
            {"distinct_lookup_data_pages", std::ranges::count(touched, 1U)},
            {"adjacent_value_page_changes", changes}, {"value_page_sequence_le64_sha256", order_digest.digest().to_hex()}};
    });
}

auto project_gather_cache_sets(const kernel::GatherU64Config& config, std::uint64_t page,
                               std::string_view workload_id, const GatherL1Geometry& cache)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    // Dividing capacity by ways first avoids a potentially overflowing lines-per-set product.
    const auto period = cache.ways ? cache.capacity_bytes / cache.ways : 0U;
    if (!cache.ways || cache.capacity_bytes % cache.ways || !period || !std::has_single_bit(period) ||
        !std::has_single_bit(cache.line_bytes) || cache.line_bytes < sizeof(std::uint64_t) ||
        period % cache.line_bytes || page % period) {
        return std::unexpected{core::Error{
            .category = core::ErrorCategory::scenario, .code = core::StableErrorCode{"scenario.cli.gather_cache_sets.geometry"},
            .message = "The L1 cache set index must fit entirely within a page-aligned input's offset",
            .operation = "scenario.resolve", .os_error = {}, .context = nlohmann::json::object(),
            .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_command}};
    }
    const auto sets = period / cache.line_bytes;
    return inspect_input(config, page, [&](auto input, const auto& digest, std::size_t index_offset) {
        // Mark each logical line once. A line shared by both arrays counts once in their union.
        std::vector<std::uint8_t> touched((input.size() - 1U) / cache.line_bytes + 1U);
        for (std::size_t offset = index_offset; offset < input.size(); offset += sizeof(std::uint64_t)) {
            touched[value_offset(input, offset) / cache.line_bytes] |= 1U;
            touched[offset / cache.line_bytes] |= 2U;
        }
        struct Counts { std::size_t value{}, indices{}, total{}; };
        std::map<std::uint64_t, Counts> counts;
        for (std::size_t line = 0; line < touched.size(); ++line) {
            if (!touched[line]) continue;
            auto& bin = counts[line % sets];
            bin.value += static_cast<std::size_t>((touched[line] & 1U) != 0U);
            bin.indices += static_cast<std::size_t>((touched[line] & 2U) != 0U);
            ++bin.total;
        }
        auto groups = nlohmann::ordered_json::array();
        std::size_t peak{}, above{}, total{};
        for (const auto& [group, bin] : counts) {
            groups.push_back({{"set_index", group}, {"indexed_value_lines", bin.value},
                              {"index_stream_lines", bin.indices}, {"distinct_lookup_lines", bin.total}});
            total += bin.total;
            peak = std::max(peak, bin.total);
            above += static_cast<std::size_t>(bin.total > cache.ways);
        }
        return nlohmann::ordered_json{
            {"schema", "epycbench.gather-cache-sets/v1"}, {"scope", "one-logical-repetition"},
            {"model", "page-offset-modulo-line-index"}, {"workload", workload_id},
            {"worker", cache.worker_id}, {"l1_cache", cache.cache_id}, {"base_page_bytes", page},
            {"capacity_bytes", cache.capacity_bytes}, {"line_bytes", cache.line_bytes}, {"ways", cache.ways},
            {"sets", sets}, {"input_sha256", digest.to_hex()}, {"indexed_lookups", config.index_count().value()},
            {"distinct_lookup_lines", total}, {"maximum_distinct_lines_per_set", peak},
            {"sets_exceeding_ways", above}, {"used_sets", std::move(groups)}};
    });
}

} // namespace epycbench::kernel::detail::gather_inspection
