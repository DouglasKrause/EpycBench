#pragma once

#include "epycbench/kernel/task.hpp"

#include <algorithm>

namespace epycbench::kernel {

inline constexpr std::string_view bfs_u64_task_id = "task.graph.bfs.u64/v1";
inline constexpr std::string_view bfs_u64_scalar_implementation_id = "scalar-u64-bfs/v1";
inline constexpr std::string_view bfs_u64_unroll4_implementation_id = "unroll4-u64-bfs/v1";
inline constexpr std::string_view bfs_u64_generator_profile =
    "epycbench.generator.mix64-permuted-bfs/v1";
inline constexpr std::string_view bfs_u64_correctness_profile =
    "epycbench.task.graph-bfs-u64.levels-fifo/v1";
inline constexpr std::uint64_t bfs_u64_input_magic = UINT64_C(0x3130305346424245);
inline constexpr std::uint64_t bfs_u64_maximum_vertices = 65536U;
inline constexpr std::size_t bfs_u64_header_words = 4U;
inline constexpr std::byte bfs_u64_padding_sentinel{0xa5};

[[nodiscard]] constexpr auto bfs_u64_degree(std::uint64_t position, std::uint64_t length) noexcept {
    return std::min(1U + position % 5U, length - 1U);
}
[[nodiscard]] constexpr auto bfs_u64_component_edges(std::uint64_t length) noexcept {
    std::uint64_t edges{};
    for (std::uint64_t i{}; i < 5U; ++i)
        edges += (length / 5U + (i < length % 5U)) * bfs_u64_degree(i, length);
    return edges;
}

class BfsU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t vertices, std::uint64_t seed = 0U)
        -> std::expected<BfsU64Config, core::Error>;
    [[nodiscard]] auto vertex_count() const noexcept -> std::uint64_t { return vertices_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto source_vertex() const noexcept -> std::uint64_t { return source_; }
    [[nodiscard]] auto stored_edges() const noexcept -> std::uint64_t { return edges_; }
    [[nodiscard]] auto visited_vertices() const noexcept -> std::uint64_t {
        return 3U * vertices_ / 4U;
    }
    [[nodiscard]] auto scanned_edges() const noexcept -> std::uint64_t { return scanned_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount {
        return *ElementCount::create(visited_vertices());
    }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U *
                                                (bfs_u64_header_words + vertices_ + 1U + edges_));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto output_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (bfs_u64_header_words + 2U + 2U * vertices_));
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / scanned_;
    }
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<TaskWork, core::Error>;
    auto operator<=>(const BfsU64Config&) const = default;

  private:
    BfsU64Config(std::uint64_t vertices, std::uint64_t seed, std::uint64_t source,
                 std::uint64_t edges, std::uint64_t scanned) noexcept
        : vertices_{vertices}, seed_{seed}, source_{source}, edges_{edges}, scanned_{scanned} {}
    std::uint64_t vertices_, seed_, source_, edges_, scanned_;
};

class BfsU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const BfsU64Config& { return config_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& {
        return output_;
    }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept
        -> std::uint64_t {
        return answer_ * count.value();
    }

  private:
    BfsU64Reference(BfsU64Config config, core::Sha256Digest input, core::Sha256Digest output,
                    std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    BfsU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_bfs_u64_reference(const BfsU64Config&, std::span<const std::byte>,
                                          std::span<std::byte>)
        -> std::expected<BfsU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_bfs_u64_input(const BfsU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_bfs_u64_reference(const BfsU64Config&, std::span<const std::byte>,
                                             std::span<std::byte>)
    -> std::expected<BfsU64Reference, core::Error>;
[[nodiscard]] auto reset_bfs_u64_output(const BfsU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_bfs_u64_output(const BfsU64Reference&, std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
