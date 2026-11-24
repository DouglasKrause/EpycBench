#include "epycbench/kernel/search_tree_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <bit>
#include <functional>

namespace epycbench::kernel {
namespace {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.search_tree_u64." + std::string{reason}},
            .message = "Threshold search validation failed",
            .operation = "kernel.task.search_tree_u64",
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
constexpr auto threshold(std::uint64_t i) noexcept -> std::uint64_t {
    return (i + 1U) * 4096U + (109U * i) % 701U;
}
auto canonical_ordinal(const SearchTreeU64Config& c, std::uint64_t position) noexcept
    -> std::uint64_t {
    if (c.query_order() == SearchTreeU64Order::grouped)
        return position;
    std::uint64_t reversed{};
    for (auto bit = c.query_count(); bit > 1U; bit >>= 1U) {
        reversed = (reversed << 1U) | (position & 1U);
        position >>= 1U;
    }
    const auto salt = detail::mix64_v1(c.input_seed() ^ UINT64_C(0xd1b54a32d192ed03));
    return reversed ^ (salt & (c.query_count() - 1U));
}
auto query(const SearchTreeU64Config& c, std::uint64_t ordinal) noexcept -> std::uint64_t {
    const auto group_size = c.query_count() / search_tree_u64_leaves;
    const auto leaf = ordinal / group_size;
    const auto within = ordinal % group_size;
    const auto low = leaf ? threshold(leaf - 1U) + 1U : 0U;
    const auto high = leaf + 1U == search_tree_u64_leaves ? 65535U : threshold(leaf);
    if (within == 0U)
        return low;
    if (within == 1U)
        return high;
    return low + detail::mix64_v1(c.input_seed() + ordinal) % (high - low + 1U);
}
template <class Accept> void canonical_input(const SearchTreeU64Config& c, Accept accept) {
    accept(0U, search_tree_u64_input_magic);
    accept(1U, c.query_count());
    for (std::size_t i = 0; i + 1U < search_tree_u64_leaves; ++i)
        accept(2U + i, threshold(i));
    for (std::uint64_t i = 0; i < c.query_count(); ++i)
        accept(search_tree_u64_header_words + i, query(c, canonical_ordinal(c, i)));
}
template <class Accept>
auto expected_leaves(const SearchTreeU64Config& c, Accept accept) -> std::uint64_t {
    const auto group_size = c.query_count() / search_tree_u64_leaves;
    std::uint64_t folded{};
    for (std::uint64_t i = 0; i < c.query_count(); ++i) {
        const auto leaf = canonical_ordinal(c, i) / group_size;
        accept(i, leaf);
        folded += (i + 1U) * (leaf + 1U);
    }
    return folded;
}
} // namespace

auto parse_search_tree_u64_order(std::string_view token) noexcept -> std::optional<SearchTreeU64Order> {
    if (token == "grouped") return SearchTreeU64Order::grouped;
    if (token == "permuted") return SearchTreeU64Order::permuted;
    return std::nullopt;
}
auto search_tree_u64_order_token(SearchTreeU64Order order) noexcept -> std::string_view {
    switch (order) {
    case SearchTreeU64Order::grouped:
        return "grouped";
    case SearchTreeU64Order::permuted:
        return "permuted";
    }
    return {};
}
auto SearchTreeU64Config::create(std::uint64_t queries, SearchTreeU64Order order,
                                 std::uint64_t seed)
    -> std::expected<SearchTreeU64Config, core::Error> {
    if (queries < search_tree_u64_leaves || queries > search_tree_u64_maximum_queries ||
        !std::has_single_bit(queries))
        return std::unexpected{error("invalid_query_count")};
    if (search_tree_u64_order_token(order).empty())
        return std::unexpected{error("invalid_query_order")};
    return SearchTreeU64Config{queries, order, seed};
}
auto SearchTreeU64Config::work(TaskRepetitionCount r) const
    -> std::expected<TaskWork, core::Error> {
    if (r.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return TaskWork::create(element_count(), r);
}
auto initialize_search_tree_u64_input(const SearchTreeU64Config& c, Output buf)
    -> std::expected<void, core::Error> {
    if (buf.size() < c.logical_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_input(c, [&](auto i, auto value) { write(buf, i, value); });
    return {};
}
auto prepare_search_tree_u64_reference(const SearchTreeU64Config& c, Input input, Output output)
    -> std::expected<SearchTreeU64Reference, core::Error> {
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
    const auto folded = expected_leaves(c, [&](auto i, auto leaf) { write(output, i, leaf); });
    std::ranges::fill(output.subspan(c.output_bytes().value()), search_tree_u64_padding_sentinel);
    return SearchTreeU64Reference{c, core::sha256(input.first(c.logical_bytes().value())),
                                  core::sha256(output.first(c.output_bytes().value())), folded};
}
auto reset_search_tree_u64_output(const SearchTreeU64Reference& ref, Output buf)
    -> std::expected<void, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    std::ranges::fill(buf.first(c.output_bytes().value()), std::byte{0xff});
    std::ranges::fill(buf.subspan(c.output_bytes().value()), search_tree_u64_padding_sentinel);
    return {};
}
auto verify_search_tree_u64_output(const SearchTreeU64Reference& ref, Input buf)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    bool matches = true;
    expected_leaves(c, [&](auto i, auto leaf) { matches &= read(buf, i) == leaf; });
    const bool padding = std::ranges::all_of(buf.subspan(c.output_bytes().value()), [](auto value) {
        return value == search_tree_u64_padding_sentinel;
    });
    return XorU64Verification{c.element_count(), ref.expected_output_digest(),
                              core::sha256(buf.first(c.output_bytes().value())), matches, padding};
}
} // namespace epycbench::kernel
