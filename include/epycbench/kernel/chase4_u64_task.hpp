#pragma once

#include "epycbench/kernel/task.hpp"
#include <array>
#include <limits>

namespace epycbench::kernel {
inline constexpr std::string_view chase4_u64_task_id = "task.chase4.u64/v1";
inline constexpr std::string_view chase4_u64_serialized_implementation_id =
    "scalar-u64-chase4-serialized/v1";
inline constexpr std::string_view chase4_u64_interleaved_implementation_id =
    "scalar-u64-chase4-interleaved/v1";
inline constexpr std::string_view chase4_u64_generator_profile =
    "epycbench.generator.four-mix64-permuted-cycles/v1";
inline constexpr std::string_view chase4_u64_correctness_profile =
    "epycbench.task.chase4-u64.canonical-terminals/v1";
inline constexpr std::uint64_t chase4_u64_lane_count = 4U;
inline constexpr std::uint64_t chase4_u64_node_bytes = 64U;
inline constexpr std::uint64_t chase4_u64_seed_stride = UINT64_C(0x9e3779b97f4a7c15);
inline constexpr std::uint64_t chase4_u64_maximum_nodes_per_lane =
    static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) / 256U;

struct Chase4U64Work {
    TaskRepetitionCount completed_tasks;
    core::WorkCount dependent_loads;
    auto operator==(const Chase4U64Work&) const -> bool = default;
};
class Chase4U64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t nodes_per_lane, std::uint64_t input_seed = 0U)
        -> std::expected<Chase4U64Config, core::Error>;
    [[nodiscard]] auto nodes_per_lane() const noexcept -> ElementCount { return nodes_; }
    [[nodiscard]] auto hops_per_lane() const noexcept -> core::WorkCount {
        return core::WorkCount::from_base_units(nodes_.value() + 1U);
    }
    // The common Task count is the number of dependent loads, not the input size.
    [[nodiscard]] auto element_count() const noexcept -> ElementCount {
        return *ElementCount::create(chase4_u64_lane_count * hops_per_lane().value());
    }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(nodes_.value() * 256U);
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / element_count().value();
    }
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<Chase4U64Work, core::Error>;
    auto operator<=>(const Chase4U64Config&) const = default;

  private:
    Chase4U64Config(ElementCount nodes, std::uint64_t seed) noexcept : nodes_{nodes}, seed_{seed} {}
    ElementCount nodes_;
    std::uint64_t seed_;
};
class Chase4U64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const Chase4U64Config& {
        return configuration_;
    }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto terminals() const noexcept -> const std::array<std::uint64_t, 4>& {
        return terminals_;
    }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept
        -> std::uint64_t {
        return answer_ * count.value();
    }

  private:
    Chase4U64Reference(Chase4U64Config config, core::Sha256Digest input,
                       std::array<std::uint64_t, 4> terminals, std::uint64_t answer) noexcept
        : configuration_{config}, input_{input}, terminals_{terminals}, answer_{answer} {}
    Chase4U64Config configuration_;
    core::Sha256Digest input_;
    std::array<std::uint64_t, 4> terminals_;
    std::uint64_t answer_;
    friend auto prepare_chase4_u64_reference(const Chase4U64Config&, std::span<const std::byte>)
        -> std::expected<Chase4U64Reference, core::Error>;
};
// Four disjoint N-node cycles. Each 64-byte node contains a LE uint64 global
// successor index and 56 zero bytes. Lane l uses canonical_latency_permutation
// with seed mix64/v1(input_seed + (l+1)*seed_stride), modulo 2^64.
// Only the logical prefix is input; allocation padding is neither read nor work.
[[nodiscard]] auto initialize_chase4_u64_input(const Chase4U64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
// Check the entire canonical prefix outside timing. Terminal derivation locates
// node zero in each canonical permutation: N+1 hops end at its successor.
// The per-task answer is sum(terminal[l]*(2*l+1)) modulo 2^64; R folds answers.
// Execution additionally requires 64-byte base alignment and unchanged bytes.
// This content reference owns no borrowed memory and makes no placement claim.
[[nodiscard]] auto prepare_chase4_u64_reference(const Chase4U64Config&, std::span<const std::byte>)
    -> std::expected<Chase4U64Reference, core::Error>;
} // namespace epycbench::kernel
