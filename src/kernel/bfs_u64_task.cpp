#include "epycbench/kernel/bfs_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>

namespace epycbench::kernel {
namespace {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.bfs_u64." + std::string{reason}},
            .message = "Breadth-first traversal validation failed",
            .operation = "kernel.task.bfs_u64",
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
auto header(const BfsU64Config& c) noexcept {
    return std::array{bfs_u64_input_magic, c.vertex_count(), c.stored_edges(), c.source_vertex()};
}
struct Labels {
    std::uint64_t mask, step, offset, inverse_step;
    explicit Labels(const BfsU64Config& c)
        : mask{c.vertex_count() - 1U},
          step{detail::mix64_v1(c.input_seed() ^ UINT64_C(0xd1b54a32d192ed03)) | 1U},
          offset{c.source_vertex()}, inverse_step{1U} {
        // An odd step is invertible modulo 2^k. Each iteration doubles the correct bits.
        for (unsigned i{}; i < 6U; ++i)
            inverse_step *= 2U - step * inverse_step;
    }
    auto physical(std::uint64_t logical) const noexcept { return (step * logical + offset) & mask; }
    auto logical(std::uint64_t physical) const noexcept {
        return (inverse_step * (physical - offset)) & mask;
    }
};
template <typename Accept> void canonical_input(const BfsU64Config& c, Accept accept) {
    const auto hdr = header(c);
    for (std::size_t i{}; i < hdr.size(); ++i)
        accept(i, hdr[i]);
    const Labels labels{c};
    const auto vertices = c.vertex_count(), reached = c.visited_vertices();
    const auto edge_start = hdr.size() + vertices + 1U;
    std::uint64_t offset{};
    for (std::uint64_t row{}; row < vertices; ++row) {
        accept(hdr.size() + row, offset);
        const auto logical = labels.logical(row);
        const auto base = logical < reached ? 0U : reached;
        const auto length = logical < reached ? reached : vertices - reached;
        const auto position = logical - base;
        const auto count = bfs_u64_degree(position, length);
        for (std::uint64_t j{}; j < count; ++j)
            accept(edge_start + offset++, labels.physical(base + (position + j + 1U) % length));
    }
    accept(hdr.size() + vertices, offset);
}
template <typename Accept>
auto canonical_output(const BfsU64Config& c, Accept accept) -> std::uint64_t {
    const auto hdr = header(c);
    for (std::size_t i{}; i < hdr.size(); ++i)
        accept(i, hdr[i]);
    const Labels labels{c};
    const auto reached = c.visited_vertices(), vertices = c.vertex_count();
    accept(hdr.size(), reached);
    accept(hdr.size() + 1U, c.scanned_edges());
    const auto distances = hdr.size() + 2U, queue = distances + vertices;
    // Ordered consecutive successors extend a contiguous discovered prefix in logical
    // coordinates. This reference reconstructs layers without executing a FIFO walk.
    std::uint64_t begin{}, end{1U}, level{}, fold{};
    while (begin < reached) {
        auto next = end;
        for (auto logical = begin; logical < end; ++logical) {
            const auto v = labels.physical(logical);
            accept(distances + v, level);
            accept(queue + logical, v);
            fold += (v + 1U) * (level + 1U);
            next =
                std::max(next, std::min(reached, logical + bfs_u64_degree(logical, reached) + 1U));
        }
        begin = end;
        end = next;
        ++level;
    }
    for (auto logical = reached; logical < vertices; ++logical) {
        accept(distances + labels.physical(logical), UINT64_MAX);
        accept(queue + logical, UINT64_MAX);
    }
    return fold;
}
} // namespace
auto BfsU64Config::create(std::uint64_t vertices, std::uint64_t seed)
    -> std::expected<BfsU64Config, core::Error> {
    if (vertices < 8U || vertices > bfs_u64_maximum_vertices || !std::has_single_bit(vertices))
        return std::unexpected{error("invalid_vertex_count")};
    const auto reached = 3U * vertices / 4U, scanned = bfs_u64_component_edges(reached);
    const auto source = detail::mix64_v1(seed ^ UINT64_C(0x8cb92baa3f3d8dd7)) & (vertices - 1U);
    return BfsU64Config{vertices, seed, source,
                        scanned + bfs_u64_component_edges(vertices - reached), scanned};
}
auto BfsU64Config::work(TaskRepetitionCount r) const -> std::expected<TaskWork, core::Error> {
    if (r.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return TaskWork::create(element_count(), r);
}
auto initialize_bfs_u64_input(const BfsU64Config& c, Output buf)
    -> std::expected<void, core::Error> {
    if (buf.size() < c.logical_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_input(c, [&](auto i, auto value) { write(buf, i, value); });
    return {};
}
auto prepare_bfs_u64_reference(const BfsU64Config& c, Input input, Output output)
    -> std::expected<BfsU64Reference, core::Error> {
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
    const auto folded = canonical_output(c, [&](auto i, auto value) { write(output, i, value); });
    std::ranges::fill(output.subspan(c.output_bytes().value()), bfs_u64_padding_sentinel);
    return BfsU64Reference{c, core::sha256(input.first(c.logical_bytes().value())),
                           core::sha256(output.first(c.output_bytes().value())), folded};
}
auto reset_bfs_u64_output(const BfsU64Reference& ref, Output buf)
    -> std::expected<void, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_output(c, [&](auto i, auto value) { write(buf, i, ~value); });
    std::ranges::fill(buf.subspan(c.output_bytes().value()), bfs_u64_padding_sentinel);
    return {};
}
auto verify_bfs_u64_output(const BfsU64Reference& ref, Input buf)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    bool matches = true;
    canonical_output(c, [&](auto i, auto value) { matches &= read(buf, i) == value; });
    const bool padding = std::ranges::all_of(buf.subspan(c.output_bytes().value()), [](auto value) {
        return value == bfs_u64_padding_sentinel;
    });
    return XorU64Verification{c.element_count(), ref.expected_output_digest(),
                              core::sha256(buf.first(c.output_bytes().value())), matches, padding};
}
} // namespace epycbench::kernel
